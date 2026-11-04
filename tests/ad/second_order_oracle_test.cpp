// Second-order Greeks by forward over reverse against references, with the repo's tolerance rule
// (src/oracle/tolerance.hpp), at a development seed, never 1729 or 271828.
//
// Check 1: the European's Black-Scholes gamma, vanna and volga, in closed form.
// Check 2: the geometric Asian's, by nested forward-mode Duals over the closed form of
//   src/oracle/geometric_asian.hpp.
// Check 5: barrier gamma under one-step survival (discrete monitoring), against a central
//   difference of the quadrature delta of tools/quadrature; and under one-step survival with the
//   Brownian bridge (continuous monitoring, Gerstner, Harrach, Roth, arXiv 1906.11002v2,
//   Algorithm 2 with sigma' = 0), gamma, vanna and volga against nested Duals over the
//   Reiner-Rubinstein closed form of src/oracle/barrier.hpp. With them, the arithmetic Asian's
//   gamma against a second difference of quadrature prices.
//
// Both barrier estimators are unbiased for their own monitoring: one-step survival for the
// discrete price (arXiv 1804.03975v3), OSS-BB for the continuous one (arXiv 1906.11002v2,
// Theorem 2.6). Each path's estimate is a smooth function of the inputs, so its derivatives are
// unbiased for the price's, and the estimator adds no bias b. The cap on the reflection exponent
// at 700 (src/pricer/conditional_kernels.hpp) changes a path only where the reflected term is
// below 1e-300 of the forward.
//
// The family, stated before it ran: the kFamily = 19 comparisons of Rows() below, alpha_F = 0.01,
// B = 64, N = 2^20 (2^14 in a smoke build, whose numbers are not results). b is 0 for a closed
// form. For a difference of quadrature values it is the difference's own allowance, below: the
// step-halving estimate of its truncation plus the quadrature error, eps = 1e-9 per value or
// delta (references/oracle_values.csv b for O5, O6 and AA12K100), amplified by the stencil.
//
// SE ceilings, U / sqrt(N), fixed before the run. The kernels smooth the payoff over the last
// step's standard deviation, s = sigma sqrt(T / m) w, w = 1 / count for an Asian average and 1
// otherwise. With D = e^{-rT} max(S0, K), a path's second derivative whose whole scale were that
// smoothing would be gamma D / (S0 s)^2, vanna D sqrt(T) / (S0 s^2), volga D T / s^2: U is
// that. A larger SE means a broken estimator, not bad luck.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "ad/dual.hpp"
#include "ad/second_order.hpp"
#include "oracle/barrier.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/geometric_asian.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "quadrature.hpp"
#include "rng/normal.hpp"

#ifndef AADP_SMOKE_SIZE
#error "cmake/area_pricer.cmake sets AADP_SMOKE_SIZE"
#endif

namespace aadp::ad {

// The names the oracle templates call, by ADL, for the nested Duals below. They must be members
// of aadp::ad itself: ADL does not search an unnamed namespace inside it.
// NOLINTBEGIN(misc-use-internal-linkage): see above.
template <class T, std::size_t N, bool A>
Dual<T, N, A> exp(const Dual<T, N, A>& x) {
  return Exp(x);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> log(const Dual<T, N, A>& x) {
  return Log(x);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> sqrt(const Dual<T, N, A>& x) {
  return Sqrt(x);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> pow(const Dual<T, N, A>& x, const Dual<T, N, A>& y) {
  return Exp(y * Log(x));
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> pow(const Dual<T, N, A>& x, double y) {
  return Exp(y * Log(x));
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> normal_cdf(const Dual<T, N, A>& x) {
  return NormalCdf(x);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> normal_pdf(const Dual<T, N, A>& x) {
  return NormalPdf(x);
}
// NOLINTEND(misc-use-internal-linkage)

namespace {

constexpr std::uint64_t kSeed = 20261112;
constexpr std::uint32_t kBatches = 64;
constexpr std::uint64_t kPaths =
    AADP_SMOKE_SIZE ? (std::uint64_t{1} << 14) : (std::uint64_t{1} << 20);
constexpr std::size_t kFamily = 19;
constexpr double kQuadratureError = 1e-9;

using quadrature::AsianInputs;
using quadrature::AsianResolution;
using quadrature::BarrierInputs;
using quadrature::BarrierResult;
using quadrature::GaussLegendreResolution;
using quadrature::SimpsonResolution;

using Nested = Dual<Dual<double, 2>, 2>;
enum class Second : std::uint8_t { kGamma, kVanna, kVolga };

const char* Name(Second s) {
  switch (s) {
    case Second::kGamma:
      return "gamma";
    case Second::kVanna:
      return "vanna";
    case Second::kVolga:
      return "volga";
  }
  return "?";
}

struct SecondGreeks {
  double gamma = 0.0;
  double vanna = 0.0;
  double volga = 0.0;
  [[nodiscard]] double Of(Second s) const {
    switch (s) {
      case Second::kGamma:
        return gamma;
      case Second::kVanna:
        return vanna;
      case Second::kVolga:
        return volga;
    }
    return std::nan("");
  }
};

// Lane 0 is spot, lane 1 volatility, in both levels, so tangent[k].tangent[j] is d2V/dx_k dx_j.
MarketParams<Nested> NestedMarket(const pricer::Market<double>& m) {
  MarketParams<Nested> n{
      .spot = Nested(m.spot), .sigma = Nested(m.sigma), .r = m.r, .q = m.q, .maturity = m.maturity};
  n.spot.value.tangent[0] = 1.0;
  n.spot.tangent[0].value = 1.0;
  n.sigma.value.tangent[1] = 1.0;
  n.sigma.tangent[1].value = 1.0;
  return n;
}

SecondGreeks FromNested(const Nested& v) {
  return {.gamma = v.tangent[0].tangent[0],
          .vanna = v.tangent[0].tangent[1],
          .volga = v.tangent[1].tangent[1]};
}

aadp::OptionType Oracle(pricer::OptionType t) {
  return t == pricer::OptionType::kCall ? aadp::OptionType::kCall : aadp::OptionType::kPut;
}

pricer::Contract Plain(pricer::Product product, pricer::OptionType type, double strike,
                       std::uint32_t steps, bool include_spot = false) {
  pricer::Contract c;
  c.product = product;
  c.type = type;
  c.strike = strike;
  c.steps = steps;
  c.include_spot = include_spot;
  return c;
}

pricer::Contract Barrier(pricer::OptionType type, double strike, std::uint32_t steps,
                         pricer::Direction direction, pricer::Monitoring monitoring, double level) {
  pricer::Contract c = Plain(pricer::Product::kBarrier, type, strike, steps);
  c.barrier = {.direction = direction,
               .knock = pricer::Knock::kOut,
               .monitoring = monitoring,
               .treatment = monitoring == pricer::Monitoring::kContinuous
                                ? pricer::Treatment::kBridge
                                : pricer::Treatment::kOneStep,
               .level = level,
               .smoothing_width = 0.0};
  return c;
}

// Check 1's reference: Black-Scholes in closed form. Vanna = -e^{-qT} phi(d1) d2 / sigma,
// volga = S e^{-qT} phi(d1) sqrt(T) d1 d2 / sigma.
SecondGreeks BlackScholesSecond(pricer::OptionType type, double strike,
                                const pricer::Market<double>& m) {
  const MarketParams<double> p{
      .spot = m.spot, .sigma = m.sigma, .r = m.r, .q = m.q, .maturity = m.maturity};
  const BsGreeks<double> g = BlackScholes(Oracle(type), strike, p);
  const double sd = m.sigma * std::sqrt(m.maturity);
  const double d1 =
      (std::log(m.spot / strike) + ((m.r - m.q + (0.5 * m.sigma * m.sigma)) * m.maturity)) / sd;
  const double d2 = d1 - sd;
  const double weight = std::exp(-m.q * m.maturity) * aadp::normal_pdf(d1);
  return {.gamma = g.gamma,
          .vanna = -weight * d2 / m.sigma,
          .volga = m.spot * weight * std::sqrt(m.maturity) * d1 * d2 / m.sigma};
}

// Check 5's discrete reference: the GL quadrature delta, differenced at h and h / 2. D(h/2) is
// the reference. In the h^2 regime |D(h/2) - gamma| ~ |D(h) - D(h/2)| / 3, so the difference
// covers the truncation; eps on each delta adds eps / (h/2) to D(h/2) and 3 eps / h to the
// difference. b = |D(h) - D(h/2)| + 5 eps / h.
struct Differenced {
  double reference = 0.0;
  double b = 0.0;
};

BarrierInputs Inputs(const pricer::Contract& c, const pricer::Market<double>& m, double spot) {
  return {.spot = spot,
          .strike = c.strike,
          .barrier = c.barrier.level,
          .r = m.r,
          .q = m.q,
          .sigma = m.sigma,
          .maturity = m.maturity,
          .monitors = c.steps};
}

double CheckedDelta(const pricer::Contract& c, const pricer::Market<double>& m, double spot) {
  const BarrierInputs in = Inputs(c, m, spot);
  const BarrierResult gl = DiscreteDownOutGaussLegendre(in, GaussLegendreResolution{});
  const BarrierResult simpson = DiscreteDownOutSimpson(in, SimpsonResolution{});
  // The two methods share no quadrature code; eps holds at this spot only if they agree to it.
  EXPECT_LE(std::fabs(gl.delta - simpson.delta), kQuadratureError) << "spot " << spot;
  return gl.delta;
}

Differenced DiscreteBarrierGamma(const pricer::Contract& c, const pricer::Market<double>& m) {
  constexpr double kStep = 0.5;
  const auto central = [&](double h) {
    return (CheckedDelta(c, m, m.spot + h) - CheckedDelta(c, m, m.spot - h)) / (2.0 * h);
  };
  const double coarse = central(kStep);
  const double fine = central(kStep / 2.0);
  return {.reference = fine, .b = std::fabs(coarse - fine) + (5.0 * kQuadratureError / kStep)};
}

// The arithmetic Asian's reference: second differences of quadrature prices at h and h / 2.
// eps on each price adds 4 eps / (h/2)^2 to D(h/2) and 4 eps / h^2 + 16 eps / h^2 to the
// difference. b = |D(h) - D(h/2)| + 36 eps / h^2. The default resolution must agree with one
// twice as fine to eps at every spot.
double CheckedAsian(const pricer::Contract& c, const pricer::Market<double>& m, double spot) {
  const AsianInputs in{.spot = spot,
                       .strike = c.strike,
                       .r = m.r,
                       .q = m.q,
                       .sigma = m.sigma,
                       .maturity = m.maturity,
                       .steps = c.steps};
  const double value = ArithmeticAsianCall(in, AsianResolution{}).value;
  AsianResolution fine;
  fine.nodes_per_sd *= 2.0;
  EXPECT_LE(std::fabs(value - ArithmeticAsianCall(in, fine).value), kQuadratureError)
      << "spot " << spot;
  return value;
}

Differenced AsianGamma(const pricer::Contract& c, const pricer::Market<double>& m) {
  constexpr double kStep = 2.0;
  const double centre = CheckedAsian(c, m, m.spot);
  const auto second = [&](double h) {
    return (CheckedAsian(c, m, m.spot + h) - (2.0 * centre) + CheckedAsian(c, m, m.spot - h)) /
           (h * h);
  };
  const double coarse = second(kStep);
  const double fine = second(kStep / 2.0);
  return {.reference = fine,
          .b = std::fabs(coarse - fine) + (36.0 * kQuadratureError / (kStep * kStep))};
}

struct Row {
  std::string name;
  pricer::Contract contract;
  pricer::Market<double> market;
  Second greek = Second::kGamma;
  double reference = 0.0;
  double b = 0.0;
};

double Ceiling(const pricer::Contract& c, const pricer::Market<double>& m, Second s) {
  double count = 1.0;
  if (c.product == pricer::Product::kAsianArithmetic ||
      c.product == pricer::Product::kAsianGeometric) {
    count = static_cast<double>(c.steps) + (c.include_spot ? 1.0 : 0.0);
  }
  const double sd = m.sigma * std::sqrt(m.maturity / static_cast<double>(c.steps)) / count;
  const double scale = std::exp(-m.r * m.maturity) * std::max(m.spot, c.strike);
  double u = 0.0;
  switch (s) {
    case Second::kGamma:
      u = scale / ((m.spot * sd) * (m.spot * sd));
      break;
    case Second::kVanna:
      u = scale * std::sqrt(m.maturity) / (m.spot * sd * sd);
      break;
    case Second::kVolga:
      u = scale * m.maturity / (sd * sd);
      break;
  }
  return u / std::sqrt(static_cast<double>(kPaths));
}

std::vector<Row> Rows() {
  using enum pricer::Product;
  using enum pricer::OptionType;
  using enum pricer::Direction;
  using enum pricer::Monitoring;
  constexpr std::array kAll = {Second::kGamma, Second::kVanna, Second::kVolga};
  const pricer::Market<double> eu{
      .spot = 100.0, .sigma = 0.25, .r = 0.05, .q = 0.02, .maturity = 1.0};
  const pricer::Market<double> ga{
      .spot = 100.0, .sigma = 0.2, .r = 0.06, .q = 0.03, .maturity = 1.0};
  const pricer::Market<double> bgk{
      .spot = 100.0, .sigma = 0.3, .r = 0.1, .q = 0.0, .maturity = 0.2};
  const pricer::Market<double> up{
      .spot = 100.0, .sigma = 0.3, .r = 0.1, .q = 0.02, .maturity = 0.5};
  const pricer::Market<double> aa{
      .spot = 100.0, .sigma = 0.17801, .r = 0.0367, .q = 0.0, .maturity = 1.0};
  std::vector<Row> rows;

  // Check 1. Strikes away from the money, so no volga is near zero.
  for (const auto& [name, contract] :
       {std::pair{std::string("european call K130"), Plain(kEuropean, kCall, 130, 4)},
        std::pair{std::string("european put K80"), Plain(kEuropean, kPut, 80, 4)}}) {
    const SecondGreeks closed = BlackScholesSecond(contract.type, contract.strike, eu);
    const SecondGreeks nested =
        FromNested(BlackScholes(Oracle(contract.type), contract.strike, NestedMarket(eu)).value);
    for (const Second s : kAll) {
      // The adapters above are what checks 2 and 5 rest on: they must give the closed form.
      EXPECT_NEAR(nested.Of(s), closed.Of(s), 1e-12 * (1.0 + std::fabs(closed.Of(s))))
          << name << " " << Name(s);
      rows.push_back({.name = name,
                      .contract = contract,
                      .market = eu,
                      .greek = s,
                      .reference = closed.Of(s)});
    }
  }

  // Check 2.
  for (const auto& [name, contract] :
       {std::pair{std::string("geometric Asian call K110"), Plain(kAsianGeometric, kCall, 110, 12)},
        std::pair{std::string("geometric Asian put K90 with spot"),
                  Plain(kAsianGeometric, kPut, 90, 12, true)}}) {
    const SecondGreeks nested =
        FromNested(GeometricAsianValue(Oracle(contract.type), contract.strike, contract.steps,
                                       contract.include_spot, NestedMarket(ga)));
    for (const Second s : kAll) {
      rows.push_back({.name = name,
                      .contract = contract,
                      .market = ga,
                      .greek = s,
                      .reference = nested.Of(s)});
    }
  }

  // Check 5, discrete: one-step survival on the contracts of O5 and DB085 (BGK 1997), K = 100,
  // m = 50. H95's gamma is small against its SE, so H85 is there for a gamma that is not.
  for (const double level : {95.0, 85.0}) {
    const pricer::Contract c = Barrier(kCall, 100, 50, kDown, kDiscrete, level);
    const Differenced d = DiscreteBarrierGamma(c, bgk);
    rows.push_back(
        {.name = "one-step down-out call H" + std::to_string(static_cast<int>(level)) + " m50",
         .contract = c,
         .market = bgk,
         .greek = Second::kGamma,
         .reference = d.reference,
         .b = d.b});
  }
  // Check 5, continuous: OSS-BB on 12 steps, against Reiner-Rubinstein.
  {
    const pricer::Contract c = Barrier(kCall, 100, 12, kDown, kContinuous, 95);
    const SecondGreeks nested = FromNested(BarrierOptionValue(
        BarrierType::kDownOut, aadp::OptionType::kCall, 100, 95, 0.0, NestedMarket(bgk)));
    for (const Second s : kAll) {
      rows.push_back({.name = "bridge down-out call H95",
                      .contract = c,
                      .market = bgk,
                      .greek = s,
                      .reference = nested.Of(s)});
    }
  }
  {
    const pricer::Contract c = Barrier(kPut, 100, 12, kUp, kContinuous, 115);
    const SecondGreeks nested = FromNested(BarrierOptionValue(
        BarrierType::kUpOut, aadp::OptionType::kPut, 100, 115, 0.0, NestedMarket(up)));
    rows.push_back({.name = "bridge up-out put H115",
                    .contract = c,
                    .market = up,
                    .greek = Second::kGamma,
                    .reference = nested.gamma});
  }
  // The arithmetic Asian AA12K100 (Fusai and Meucci 2008), against quadrature.
  {
    const pricer::Contract c = Plain(kAsianArithmetic, kCall, 100, 12, true);
    const Differenced d = AsianGamma(c, aa);
    rows.push_back({.name = "arithmetic Asian AA12K100",
                    .contract = c,
                    .market = aa,
                    .greek = Second::kGamma,
                    .reference = d.reference,
                    .b = d.b});
  }
  return rows;
}

const pricer::Estimate& Pick(const HessianEstimate<2>& e, Second s) {
  switch (s) {
    case Second::kGamma:
      return e.rows[0][kSpotInput];
    case Second::kVanna:
      return e.rows[0][kVolInput];
    case Second::kVolga:
      return e.rows[1][kVolInput];
  }
  return e.rows[0][kSpotInput];
}

TEST(SecondOrderOracle, GammaVannaAndVolgaAgreeWithTheirReferences) {
  const std::vector<Row> rows = Rows();
  ASSERT_EQ(rows.size(), kFamily);
  const double critical = CriticalValue(kFamilyWiseAlpha, kFamily, kBatches);
  const pricer::RunConfig config{.seed = kSeed, .paths = kPaths, .batches = kBatches, .threads = 2};
  // Runs are shared by the rows of one contract and market.
  const Row* last = nullptr;
  std::expected<HessianEstimate<2>, SecondOrderError> estimate;
  for (const Row& row : rows) {
    if (last == nullptr || last->name != row.name) {
      estimate = PriceWithHessian<2>(row.contract, row.market, config, {kSpotInput, kVolInput});
      last = &row;
    }
    ASSERT_TRUE(estimate.has_value()) << row.name;
    const pricer::Estimate& e = Pick(*estimate, row.greek);
    const ToleranceLimits limits{.b = row.b,
                                 .se_ceiling = Ceiling(row.contract, row.market, row.greek),
                                 .critical = critical};
    const ToleranceResult r = CheckAgainstReference(e.batch_means, row.reference, limits);
    std::printf(
        "%s, %s, %s: estimate %.6g reference %.6g |diff| %.3g bound %.3g (b %.3g) SE %.3g "
        "ceiling %.3g: %s\n",
        AADP_SMOKE_SIZE ? "smoke, not a result" : "measured", row.name.c_str(), Name(row.greek),
        r.estimate, r.reference, std::fabs(r.estimate - r.reference), r.bound, row.b,
        r.standard_error, r.se_ceiling, r.passed() ? "pass" : r.failed_condition());
    EXPECT_TRUE(r.passed()) << row.name << " " << Name(row.greek) << ": " << r.failed_condition();
  }
}

}  // namespace
}  // namespace aadp::ad
