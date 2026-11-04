// Hand-written pathwise adjoint of the GBM arithmetic Asian and European, with no tape: the
// baseline the tape is timed against (BENCHMARKS.md AAD-5 and AAD-6).
//
//   PriceWithHandAdjoint(contract, market, {seed, paths, batches, threads})
//       -> GradientEstimate, as PriceWithGradient gives it, or why it refused
//
// Reverse mode written out by hand, in the style of Giles and Glasserman, "Smoking adjoints: fast
// Monte Carlo Greeks", Risk, January 2006 (Oxford report NA-05-15, sections 3 and 4): a forward
// pass over the path that keeps what the reverse pass reads, then the reverse pass over the steps,
// last first, then the adjoint of the set-up. Written from the pricer's own operations.
//
// Forward. Operation for operation the double kernels of pricer/kernels.hpp, AsianArithmetic and
// European, on the model's own drift mu and sd s, with src/math/ inverse normal and exp:
//   x_0 = log S0,  x_i = (x_{i-1} + mu) + s z_i,  S_i = exp x_i,  i = 1..m,
//   Asian A = (S_0 if averaged + S_1 + ... + S_m) / n,  European A = S_m,
//   V = D max(+-(A - K), 0),  D = exp(-r T),
// so the value is DiscountedPayoff<double>'s bit for bit, on the same draws.
//
// Reverse, on a path in the money (on any other every adjoint is zero), with c = +-1:
//   Asian     sbar = c D / n,  xbar_{m+1} = 0,  for i = m..1: xbar_i = xbar_{i+1} + sbar S_i,
//   European  xbar_i = c D S_m for every i, since x_i reaches V only through x_{i+1},
//   then for i = m..1: sbar_sd += xbar_i z_i and mubar += xbar_i, and log S0 bar += xbar_1,
//   S0 bar += sbar when S0 is averaged, and Dbar += max(+-(A - K), 0).
// Those five adjoints of the model's members add up over a block of paths (Boundary), and the
// set-up's adjoint (MarketAdjoint) takes them to the five market inputs once per block, as the
// tape's set-up sweep does (gradient.hpp). The set-up is the model's:
//   log S0;  D = exp((-r) T);  mu = a h, a = (r - q) - (0.5 sigma) sigma, h = T / m;
//   s = sigma sqrt(h), h computed again;
// and every partial is the double the tape records for it (real.hpp), so hand and tape differ
// only in association. The per-path accumulators even take their terms in the tape's order;
// only the set-up's adjoint groups them differently. tests/ad/dual_agreement_test.cpp bounds the
// difference, per path and per batch.
//
// What makes it fast, short of changing the arithmetic:
//   - no tape, no indices, no virtual call or std::function: one inline function per path, a
//     template on the product, so RunBatches inlines it into its loop;
//   - the forward pass stores only S_i and z_i (the European only z_i) in two buffers per worker,
//     allocated once, and the reverse pass reads them back as three running sums in registers;
//   - a path out of the money skips the reverse pass, because every adjoint on it is zero. The
//     tape sweeps such a path anyway (it has no zero-adjoint skip, EVIDENCE B9), so the hand
//     adjoint's cost falls with the fraction of paths in the money, which a timing must report;
//   - the set-up's adjoint runs once per block of 1,024 paths, on the block's sums, and its
//     partials are computed once per run.
// It is still the reverse mode the brief asks for. For a one-factor model the same derivative
// also comes out of a forward pass alone, as running sums of S_i, i S_i and S_i (z_1 + ... + z_i),
// with nothing stored; that would be the pathwise tangent method, not an adjoint, and is not
// what AAD-5 compares.
#ifndef AADP_AD_HAND_ADJOINT_HPP_
#define AADP_AD_HAND_ADJOINT_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "ad/gradient.hpp"
#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/sqrt.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::ad::hand {

// What a path reads, and the partials of the set-up's adjoint, for one contract and market.
struct Setup {
  bool asian = false;  // arithmetic Asian, else European
  bool call = true;
  bool include_spot = false;
  std::uint32_t steps = 1;
  double strike = 0.0;
  // The model's members, from pricer::GbmModel<double> itself.
  double spot = 0.0;
  double log_spot = 0.0;
  double discount = 0.0;
  double drift = 0.0;
  double sd = 0.0;
  // The Asian's divisor n and the partial 1 / n of the average.
  double count = 1.0;
  double inv_count = 1.0;
  // Partials of the set-up, each the double the tape records.
  double inv_spot = 0.0;        // d log S0 / d S0
  double maturity = 0.0;        // d((-r) T) / d(-r)
  double minus_r = 0.0;         // d((-r) T) / d T
  double drift_rate = 0.0;      // a = r - q - sigma^2 / 2, d mu / d h
  double step = 0.0;            // h = T / m, d mu / d a
  double inv_steps = 0.0;       // d h / d T
  double sigma = 0.0;           // d((0.5 sigma) sigma) / d(0.5 sigma), and d s / d sqrt(h)
  double half_sigma = 0.0;      // d((0.5 sigma) sigma) / d sigma
  double sqrt_step = 0.0;       // d s / d sigma
  double half_over_sqrt = 0.0;  // d sqrt(h) / d h
};

// The products this file prices.
constexpr bool Supported(const pricer::Contract& c) {
  return c.product == pricer::Product::kAsianArithmetic || c.product == pricer::Product::kEuropean;
}

// Precondition: Supported(c) and pricer::Validate(c, m) passed.
inline Setup MakeSetup(const pricer::Contract& c, const pricer::Market<double>& m) {
  const pricer::GbmModel<double> model(m, c.steps);
  const auto steps = static_cast<double>(c.steps);
  Setup s;
  s.asian = c.product == pricer::Product::kAsianArithmetic;
  s.call = c.type == pricer::OptionType::kCall;
  s.include_spot = c.include_spot;
  s.steps = c.steps;
  s.strike = c.strike;
  s.spot = model.Spot();
  s.log_spot = model.LogSpot();
  s.discount = model.Discount();
  s.drift = model.Drift();
  s.sd = model.StepSd();
  s.count = steps + (c.include_spot ? 1.0 : 0.0);
  s.inv_count = 1.0 / s.count;
  s.inv_spot = 1.0 / m.spot;
  s.maturity = m.maturity;
  s.minus_r = -m.r;
  s.drift_rate = m.r - m.q - 0.5 * m.sigma * m.sigma;
  s.step = m.maturity / steps;
  s.inv_steps = 1.0 / steps;
  s.sigma = m.sigma;
  s.half_sigma = 0.5 * m.sigma;
  s.sqrt_step = math::sqrt(m.maturity / steps);
  s.half_over_sqrt = 0.5 / s.sqrt_step;
  return s;
}

// Adjoints of the model's members, summed over the paths since the last clear.
struct Boundary {
  double spot = 0.0;  // S0 where the Asian averages it, not through log S0
  double log_spot = 0.0;
  double discount = 0.0;
  double drift = 0.0;
  double sd = 0.0;
};

// What the forward pass keeps for the reverse pass, one per worker.
class Scratch {
 public:
  explicit Scratch(std::uint32_t steps) : spots_(steps), normals_(steps) {}
  [[nodiscard]] double* spots() { return spots_.data(); }
  [[nodiscard]] double* normals() { return normals_.data(); }

 private:
  std::vector<double> spots_;
  std::vector<double> normals_;
};

// One path of the arithmetic Asian: returns its discounted payoff and adds its adjoints to acc.
// Precondition: u.size() == setup.steps, and scratch was made for that many steps.
inline double AsianPath(const Setup& s, std::span<const double> u, Scratch& scratch,
                        Boundary& acc) {
  double* spots = scratch.spots();
  double* normals = scratch.normals();
  double x = s.log_spot;
  double sum = s.include_spot ? s.spot : 0.0;
  for (std::uint32_t i = 0; i < s.steps; ++i) {
    const double z = math::inverse_normal(u[i]);
    x = (x + s.drift) + s.sd * z;
    const double spot = math::exp(x);
    sum = sum + spot;
    spots[i] = spot;
    normals[i] = z;
  }
  const double average = sum / s.count;
  const double intrinsic = s.call ? average - s.strike : s.strike - average;
  const bool in_money = intrinsic > 0.0;
  const double payoff = in_money ? intrinsic : 0.0;
  acc.discount += payoff;
  if (in_money) {
    const double sbar = (s.call ? s.discount : -s.discount) * s.inv_count;
    double xbar = 0.0;
    double drift = acc.drift;
    double sd = acc.sd;
    for (std::uint32_t i = s.steps; i-- > 0;) {
      xbar = xbar + sbar * spots[i];
      sd = sd + xbar * normals[i];
      drift = drift + xbar;
    }
    acc.drift = drift;
    acc.sd = sd;
    acc.log_spot += xbar;
    if (s.include_spot) acc.spot += sbar;
  }
  return s.discount * payoff;
}

// One path of the European, as AsianPath.
inline double EuropeanPath(const Setup& s, std::span<const double> u, Scratch& scratch,
                           Boundary& acc) {
  double* normals = scratch.normals();
  double x = s.log_spot;
  for (std::uint32_t i = 0; i < s.steps; ++i) {
    const double z = math::inverse_normal(u[i]);
    x = (x + s.drift) + s.sd * z;
    normals[i] = z;
  }
  const double spot = math::exp(x);
  const double intrinsic = s.call ? spot - s.strike : s.strike - spot;
  const bool in_money = intrinsic > 0.0;
  const double payoff = in_money ? intrinsic : 0.0;
  acc.discount += payoff;
  if (in_money) {
    const double xbar = (s.call ? s.discount : -s.discount) * spot;
    double drift = acc.drift;
    double sd = acc.sd;
    for (std::uint32_t i = s.steps; i-- > 0;) {
      sd = sd + xbar * normals[i];
      drift = drift + xbar;
    }
    acc.drift = drift;
    acc.sd = sd;
    acc.log_spot += xbar;
  }
  return s.discount * payoff;
}

// The set-up's adjoint: dV/d(S0, sigma, r, q, T) from the members' adjoints. Theta is -dV/dT.
inline std::array<double, kInputs> MarketAdjoint(const Setup& s, const Boundary& b) {
  // D = exp(n), n = (-r) T.
  const double n = b.discount * s.discount;
  double r = -(n * s.maturity);
  double maturity = n * s.minus_r;
  // mu = a h, a = (r - q) - (0.5 sigma) sigma, h = T / m.
  const double a = b.drift * s.step;
  maturity += (b.drift * s.drift_rate) * s.inv_steps;
  r += a;
  const double q = 0.0 - a;  // +0 when a is, as the tape's accumulator gives
  const double half = -a * s.sigma;
  double sigma = -a * s.half_sigma;
  sigma += half * 0.5;
  // s = sigma sqrt(h).
  sigma += b.sd * s.sqrt_step;
  maturity += ((b.sd * s.sigma) * s.half_over_sqrt) * s.inv_steps;
  // log S0, and S0 itself where it is averaged.
  const double spot = b.spot + (b.log_spot * s.inv_spot);
  return {spot, sigma, r, q, maturity};
}

// The RunBatches path function: statistic 0 is the payoff, 1 to 5 the Greeks, which only
// EndBlock fills, as for GradientPathFn.
template <bool kAsian>
class PathFn {
 public:
  explicit PathFn(const Setup& setup) : setup_(setup), scratch_(setup.steps) {}

  void operator()(const pricer::PathUniforms& u, std::array<double, kGradientStats>& stats) {
    stats = {};
    if constexpr (kAsian) {
      stats[0] = AsianPath(setup_, u.normal, scratch_, boundary_);
    } else {
      stats[0] = EuropeanPath(setup_, u.normal, scratch_, boundary_);
    }
  }
  void EndBlock(std::array<double, kGradientStats>& sum) {
    const std::array<double, kInputs> gradient = MarketAdjoint(setup_, boundary_);
    for (std::size_t i = 0; i < kInputs; ++i) {
      sum[1 + i] += i == kMaturityInput ? -gradient[i] : gradient[i];
    }
    boundary_ = {};
  }

 private:
  Setup setup_;
  Scratch scratch_;
  Boundary boundary_;
};

struct Refusal {
  bool unsupported_product = false;
  pricer::PriceError price;
};

inline std::expected<GradientEstimate, Refusal> PriceWithHandAdjoint(
    const pricer::Contract& contract, const pricer::Market<double>& market,
    const pricer::RunConfig& config) {
  if (!Supported(contract)) {
    return std::unexpected(Refusal{.unsupported_product = true, .price = {}});
  }
  const pricer::ContractError contract_error = pricer::Validate(contract, market);
  if (contract_error != pricer::ContractError::kNone) {
    return std::unexpected(Refusal{.price = {.contract = contract_error}});
  }
  const pricer::RunError run_error = pricer::ValidateRun(config);
  if (run_error != pricer::RunError::kNone) {
    return std::unexpected(Refusal{.price = {.run = run_error}});
  }
  const Setup setup = MakeSetup(contract, market);
  const pricer::PathShape shape{.steps = contract.steps, .crossing = false};
  if (setup.asian) {
    return EstimateFromTable(
        pricer::RunBatches<kGradientStats>(config, shape, [&] { return PathFn<true>(setup); }));
  }
  return EstimateFromTable(
      pricer::RunBatches<kGradientStats>(config, shape, [&] { return PathFn<false>(setup); }));
}

}  // namespace aadp::ad::hand

#endif  // AADP_AD_HAND_ADJOINT_HPP_
