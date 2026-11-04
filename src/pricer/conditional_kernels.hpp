// Conditional path kernels: the payoff's expectation over the last step in closed form, for the
// second-order Greeks (src/ad/second_order.hpp). New kernels beside kernels.hpp, which they do
// not change: Price() and every first-order Greek still run kernels.hpp.
//
// Why. A pathwise second derivative of max(S_T - K, 0) is zero on every path and misses the mass
// the kink puts at S_T = K, so forward over reverse on kernels.hpp gives a biased gamma. Taking
// the expectation over the last step given x_{m-1} (Glasserman, Monte Carlo Methods in Financial
// Engineering, 2004, section 7.2.3, smoothing by conditional expectation; the pricer's digital
// already does this, after Giles, "Vibrato Monte Carlo sensitivities", pp. 4-5) gives a payoff
// that is smooth in x_{m-1}, hence in every input, with the same expectation. Under exact GBM the
// last step is normal in log space, so the expectation is closed form:
//   E[(e^X - K) 1{lo < X < hi}] = F [Phi(h - s) - Phi(l - s)] - K [Phi(h) - Phi(l)],
//   X ~ N(mu, s^2), F = exp(mu + s^2 / 2), l = (lo - mu) / s, h = (hi - mu) / s,
// with an end at infinity dropped exactly (Phi = 0 or 1), and the put its negative.
//
// Barriers. The monitoring decides the estimator, and the treatment must be the one whose
// first derivatives are Lipschitz:
//   discrete, kOneStep     one-step survival (Gerstner, Harrach, Roth, arXiv 1804.03975v3) for
//                          steps 1 to m - 1, then the last date's survival inside the closed form:
//                          the region above is cut at the barrier.
//   continuous, kBridge    the one-step survival Brownian bridge (Gerstner, Harrach, Roth, arXiv
//                          1906.11002v2, section 2.1, eqs. 2.7 to 2.9, Algorithm 2 with
//                          sigma' = 0, which is exact for GBM in log space): each step is drawn on
//                          the live side as above and weighted by p (1 - exp(-2 d0 d1 / s^2)),
//                          d0, d1 > 0 the gaps at its two ends. The paper proves it unbiased
//                          against the bridge (Theorem 2.6) and its first derivatives Lipschitz.
//                          The last step's bridge factor is inside the closed form: on the live
//                          side, E[g(X) exp(-2 d0 d1(X) / s^2)] = R E[g(Y)], Y ~ N(mu +- 2 d0,
//                          s^2), R = exp(+-2 d0 (mu - x) / s^2), + for an up barrier, - for a down
//                          (completing the square).
//   kHard, kSmoothed       refused (ValidateConditional): an indicator's pathwise derivatives are
//                          zero, and the smoothed barrier is a different, biased contract.
// The plain bridge weight's Max(gap, 0) is what makes its first derivative non-Lipschitz
// (Gerstner 2019, section 2.1). Here a live path's gaps are positive by construction, so the Max
// below guards only dead paths, whose weight is already zero, and never binds on a live one.
//
// Bias added. None beyond Monte Carlo for exact GBM steps: each kernel's expectation is the
// production kernel's (the closed form is the exact conditional expectation; the one-step and
// bridge forms are unbiased by the theorems cited). Two documented exceptions: the one-step guard
// of kernels.hpp, copied unchanged with its bound, and R's exponent, capped at 700 so it cannot
// overflow. ValidateConditional keeps the drift per step within 4 standard deviations, so the cap
// binds only when d0 > 87.5 s, and then every live X that brings the bridge factor above
// exp(-d0^2 / s^2) lies beyond 39.75 s from mu: the capped term is below 1e-300 of the forward.
#ifndef AADP_PRICER_CONDITIONAL_KERNELS_HPP_
#define AADP_PRICER_CONDITIONAL_KERNELS_HPP_

#include <cstdint>
#include <span>

#include "math/log.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/real_ops.hpp"

namespace aadp::pricer {

enum class ConditionalError : std::uint8_t { kNone, kIndicator, kSmoothed, kDriftPerStep };

// For a second-order Greek of a contract Validate accepted: kNone, or why it has none here.
ConditionalError ValidateConditional(const Contract& contract, const Market<double>& market);
const char* ErrorName(ConditionalError error);

// The cap on the reflection exponent, and the drift per step, in standard deviations, that keeps
// the cap's effect below 1e-300 of the forward (above).
inline constexpr double kMaxReflectionExponent = 700.0;
inline constexpr double kMaxConditionalDriftSd = 4.0;

namespace conditional_detail {

// An interval of log S_T, fixed by the contract. lo and hi are used only where present.
struct Region {
  bool empty = false;
  bool has_lo = false;
  bool has_hi = false;
  double lo = 0.0;
  double hi = 0.0;
};

inline Region Vanilla(const Contract& c) {
  const double k = math::log(c.strike);
  if (c.type == OptionType::kCall) return {.has_lo = true, .lo = k};
  return {.has_hi = true, .hi = k};
}

// Where a knock-out pays at T: the vanilla region cut at the barrier.
inline Region KnockOutRegion(const Contract& c) {
  const double k = math::log(c.strike);
  const double b = math::log(c.barrier.level);
  const bool down = c.barrier.direction == Direction::kDown;
  if (c.type == OptionType::kCall) {
    if (down) return {.has_lo = true, .lo = k > b ? k : b};
    return {.empty = !(k < b), .has_lo = true, .has_hi = true, .lo = k, .hi = b};
  }
  if (down) return {.empty = !(b < k), .has_lo = true, .has_hi = true, .lo = b, .hi = k};
  return {.has_hi = true, .hi = k < b ? k : b};
}

// Phi(h) - Phi(l) for l <= h. Where both are above 1/2 it is taken as Phi(-l) - Phi(-h), the
// same function, since the direct difference of two numbers near 1 keeps only their rounding:
// on a path far below an up-and-out call's strike it left 1e-16 of noise in a value of 1e-20.
template <class Real>
Real Mass(const Real& l, const Real& h) {
  const auto upper = l > 0.0;
  return NormalCdf(Select(upper, Real(-l), h)) - NormalCdf(Select(upper, Real(-h), l));
}

// E[(e^X - K) 1{X in r}] for a call, E[(K - e^X) 1{X in r}] for a put, X ~ N(mean, sd^2).
template <class Real>
Real Corridor(OptionType type, double strike, const Region& r, const Real& mean, const Real& sd) {
  if (r.empty) return Real(0.0);
  const Real forward = Exp(mean + 0.5 * sd * sd);
  Real strike_mass(0.0);  // P(X in r)
  Real share_mass(0.0);   // P(X in r) under the share measure, X ~ N(mean + sd^2, sd^2)
  if (r.has_lo && r.has_hi) {
    const Real l = (r.lo - mean) / sd;
    const Real h = (r.hi - mean) / sd;
    strike_mass = Mass(l, h);
    share_mass = Mass(Real(l - sd), Real(h - sd));
  } else if (r.has_lo) {
    const Real l = (mean - r.lo) / sd;
    strike_mass = NormalCdf(l);
    share_mass = NormalCdf(l + sd);
  } else {
    const Real h = (r.hi - mean) / sd;
    strike_mass = NormalCdf(h);
    share_mass = NormalCdf(h - sd);
  }
  const Real call = forward * share_mass - strike * strike_mass;
  return type == OptionType::kCall ? call : -call;
}

template <class Real, class Model>
Real European(const Contract& c, const Model& model, std::span<const double> u) {
  Real x = model.LogSpot();
  for (std::uint32_t i = 0; i + 1 < c.steps; ++i) x = kernel_detail::Advance(model, i, x, u[i]);
  const StepLaw<Real> law = model.Law(c.steps - 1, x);
  return model.Discount() * Corridor(c.type, c.strike, Vanilla(c), law.mean, law.sd);
}

// log G = (sum of the first m - 1 log fixings + x_m) / count, so given x_{m-1} it is normal with
// mean (sum + mu) / count and standard deviation s / count.
template <class Real, class Model>
Real AsianGeometric(const Contract& c, const Model& model, std::span<const double> u) {
  Real x = model.LogSpot();
  Real sum = c.include_spot ? x : Real(0.0);
  for (std::uint32_t i = 0; i + 1 < c.steps; ++i) {
    x = kernel_detail::Advance(model, i, x, u[i]);
    sum = sum + x;
  }
  const StepLaw<Real> law = model.Law(c.steps - 1, x);
  const double count = static_cast<double>(c.steps) + (c.include_spot ? 1.0 : 0.0);
  return model.Discount() * Corridor(c.type, c.strike, Vanilla(c), Real((sum + law.mean) / count),
                                     Real(law.sd / count));
}

// (sum + S_m) / count - K = (S_m - K') / count with K' = count K - sum, the strike left for the
// last fixing. K' <= 0: the call is in the money for every S_m, E = (F - K') / count, and the put
// is worth zero. Both sides are evaluated (the kernel never branches on a Real), so the strike
// whose log is taken is 1 where K' <= 0.
template <class Real, class Model>
Real AsianArithmetic(const Contract& c, const Model& model, std::span<const double> u) {
  Real x = model.LogSpot();
  Real sum = c.include_spot ? model.Spot() : Real(0.0);
  for (std::uint32_t i = 0; i + 1 < c.steps; ++i) {
    x = kernel_detail::Advance(model, i, x, u[i]);
    sum = sum + Exp(x);
  }
  const StepLaw<Real> law = model.Law(c.steps - 1, x);
  const double count = static_cast<double>(c.steps) + (c.include_spot ? 1.0 : 0.0);
  const Real left = count * c.strike - sum;
  const auto positive = left > 0.0;
  const Real strike = Select(positive, left, Real(1.0));
  const Real forward = Exp(law.mean + 0.5 * law.sd * law.sd);
  const Real d = (law.mean - Log(strike)) / law.sd;
  const Real inner =
      c.type == OptionType::kCall
          ? Select(positive, Real(forward * NormalCdf(d + law.sd) - strike * NormalCdf(d)),
                   Real(forward - left))
          : Select(positive, Real(strike * NormalCdf(-d) - forward * NormalCdf(-(d + law.sd))),
                   Real(0.0));
  return model.Discount() * inner / count;
}

// One step of one-step survival from the step's law: kernels.hpp's OutOneStep loop body
// unchanged, guard included. Returns the next log price.
template <class Real>
Real SurvivingStep(const Contract& c, const StepLaw<Real>& law, const Real& log_barrier,
                   double uniform, Real& weight) {
  const bool down = c.barrier.direction == Direction::kDown;
  const Real p = NormalCdf(kernel_detail::Gap(c.barrier.direction, law.mean, log_barrier) / law.sd);
  const double v = down ? 1.0 - uniform : uniform;
  const Real q = p * v;
  const auto alive = q > 0.0;
  weight = weight * Select(alive, p, Real(0.0));
  const Real drawn = Value(q) < 0x1p-1000 ? Real(Value(q)) : q;
  const Real conditional = InverseNormal(Select(alive, drawn, Real(v)));
  const Real z = down ? -conditional : conditional;
  return law.mean + law.sd * z;
}

template <class Real, class Model>
Real OutOneStep(const Contract& c, const Model& model, std::span<const double> u) {
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  for (std::uint32_t i = 0; i + 1 < c.steps; ++i) {
    x = SurvivingStep(c, model.Law(i, x), log_barrier, u[i], weight);
  }
  const StepLaw<Real> law = model.Law(c.steps - 1, x);
  return model.Discount() * weight *
         Corridor(c.type, c.strike, KnockOutRegion(c), law.mean, law.sd);
}

template <class Real, class Model>
Real OutOneStepBridge(const Contract& c, const Model& model, std::span<const double> u) {
  const Direction direction = c.barrier.direction;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  for (std::uint32_t i = 0; i + 1 < c.steps; ++i) {
    const StepLaw<Real> law = model.Law(i, x);
    const Real next = SurvivingStep(c, law, log_barrier, u[i], weight);
    const Real d0 = Max(kernel_detail::Gap(direction, x, log_barrier), Real(0.0));
    const Real d1 = Max(kernel_detail::Gap(direction, next, log_barrier), Real(0.0));
    weight = weight * (1.0 - Exp(-2.0 * d0 * d1 / (law.sd * law.sd)));
    x = next;
  }
  const StepLaw<Real> law = model.Law(c.steps - 1, x);
  const Real d0 = Max(kernel_detail::Gap(direction, x, log_barrier), Real(0.0));
  const double sign = direction == Direction::kUp ? 1.0 : -1.0;
  const Real exponent = sign * 2.0 * d0 * (law.mean - x) / (law.sd * law.sd);
  const Real reflection = Exp(Min(exponent, Real(kMaxReflectionExponent)));
  const Region region = KnockOutRegion(c);
  const Real direct = Corridor(c.type, c.strike, region, law.mean, law.sd);
  const Real reflected =
      Corridor(c.type, c.strike, region, Real(law.mean + sign * 2.0 * d0), law.sd);
  return model.Discount() * weight * (direct - reflection * reflected);
}

template <class Real, class Model>
Real KnockOut(const Contract& c, const Model& model, std::span<const double> u) {
  return c.barrier.monitoring == Monitoring::kContinuous ? OutOneStepBridge<Real>(c, model, u)
                                                         : OutOneStep<Real>(c, model, u);
}

}  // namespace conditional_detail

// The discounted payoff's expectation over the last step, given the first m - 1 steps.
// Precondition: Validate(c, market) and ValidateConditional(c, market) passed; reads only
// u.normal.
template <class Real, class Model>
Real ConditionalPayoff(const Contract& c, const Model& model, const PathUniforms& u) {
  using namespace conditional_detail;
  switch (c.product) {
    case Product::kEuropean:
      return European<Real>(c, model, u.normal);
    case Product::kDigital:
      return kernel_detail::Digital<Real>(c, model, u.normal).value;
    case Product::kAsianArithmetic:
      return AsianArithmetic<Real>(c, model, u.normal);
    case Product::kAsianGeometric:
      return AsianGeometric<Real>(c, model, u.normal);
    case Product::kBarrier: {
      const Real out = KnockOut<Real>(c, model, u.normal);
      if (c.barrier.knock == Knock::kOut) return out;
      // Knock-in by parity on the same uniforms, as in kernels.hpp.
      return European<Real>(c, model, u.normal) - out;
    }
  }
  return Real(0.0);
}

}  // namespace aadp::pricer

#endif  // AADP_PRICER_CONDITIONAL_KERNELS_HPP_
