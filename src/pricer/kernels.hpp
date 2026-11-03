// Path kernels: one discounted payoff per path, templated on Real and on the model.
//
// A kernel reads the path's uniforms (draws.hpp) and the model's step law (model.hpp), and uses
// only the operations of real_ops.hpp. No kernel branches on a Real: a barrier kill, a payoff
// floor and a ramp are all Selects, so every path and every lane runs the same operations, and a
// killed path runs on with weight zero rather than returning early. Branches on the contract are
// hoisted out of the step loop.
//
// For the products the reference kernels in src/mc/gbm_kernels.hpp also price, every operation
// and its order is the same as there, so with libm's exp, log, inverse normal and normal CDF in
// place of ours the two give the same bits. tests/pricer/path_agreement_test.cpp checks that, and
// bounds the difference ours makes.
#ifndef AADP_PRICER_KERNELS_HPP_
#define AADP_PRICER_KERNELS_HPP_

#include <cstdint>
#include <span>

#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/model.hpp"
#include "pricer/real_ops.hpp"

namespace aadp::pricer {

// value is the discounted payoff. weight is the barrier survival weight the value carries: one
// for products with no barrier, the knock-out leg's for a knock-in.
template <class Real>
struct PathValue {
  Real value;
  Real weight;
};

// Only a sampled continuous crossing reads the second uniform of each step.
constexpr bool NeedsCrossingUniforms(const Contract& c) {
  return c.product == Product::kBarrier && c.barrier.monitoring == Monitoring::kContinuous &&
         c.barrier.treatment == Treatment::kHard;
}

namespace kernel_detail {

// Same expression and floor as the reference's Payoff: a floor that keeps +0, not -0.
template <class Real>
Real Intrinsic(OptionType type, const Real& underlying, double strike) {
  const Real intrinsic = type == OptionType::kCall ? underlying - strike : strike - underlying;
  return Select(intrinsic > 0.0, intrinsic, Real(0.0));
}

template <class Real, class Model>
Real Advance(const Model& model, std::uint32_t step, const Real& x, double uniform) {
  const StepLaw<Real> law = model.Law(step, x);
  return law.mean + law.sd * InverseNormal(Real(uniform));
}

template <class Real, class Model>
PathValue<Real> European(const Contract& c, const Model& model, std::span<const double> u) {
  Real x = model.LogSpot();
  for (std::uint32_t i = 0; i < c.steps; ++i) x = Advance(model, i, x, u[i]);
  return {model.Discount() * Intrinsic(c.type, Real(Exp(x)), c.strike), Real(1.0)};
}

// E[1{S_T > K} | x_{m-1}] = Phi((mean - log K) / sd) over the last step's law. The last uniform
// is not read.
template <class Real, class Model>
PathValue<Real> Digital(const Contract& c, const Model& model, std::span<const double> u) {
  Real x = model.LogSpot();
  for (std::uint32_t i = 0; i + 1 < c.steps; ++i) x = Advance(model, i, x, u[i]);
  const StepLaw<Real> law = model.Law(c.steps - 1, x);
  const Real d = (law.mean - Log(Real(c.strike))) / law.sd;
  const Real p = NormalCdf(c.type == OptionType::kCall ? d : -d);
  return {model.Discount() * p, Real(1.0)};
}

template <class Real, class Model>
PathValue<Real> AsianArithmetic(const Contract& c, const Model& model, std::span<const double> u) {
  Real x = model.LogSpot();
  Real sum = c.include_spot ? model.Spot() : Real(0.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    x = Advance(model, i, x, u[i]);
    sum = sum + Exp(x);
  }
  const double count = static_cast<double>(c.steps) + (c.include_spot ? 1.0 : 0.0);
  return {model.Discount() * Intrinsic(c.type, Real(sum / count), c.strike), Real(1.0)};
}

template <class Real, class Model>
PathValue<Real> AsianGeometric(const Contract& c, const Model& model, std::span<const double> u) {
  Real x = model.LogSpot();
  Real sum = c.include_spot ? x : Real(0.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    x = Advance(model, i, x, u[i]);
    sum = sum + x;
  }
  const double count = static_cast<double>(c.steps) + (c.include_spot ? 1.0 : 0.0);
  return {model.Discount() * Intrinsic(c.type, Real(Exp(sum / count)), c.strike), Real(1.0)};
}

// Distance into the live side, in log space: positive while the path is alive.
template <class Real>
Real Gap(Direction direction, const Real& x, const Real& log_barrier) {
  return direction == Direction::kDown ? x - log_barrier : log_barrier - x;
}

template <class Real, class Model>
PathValue<Real> Finish(const Contract& c, const Model& model, const Real& x, const Real& weight) {
  return {model.Discount() * weight * Intrinsic(c.type, Real(Exp(x)), c.strike), weight};
}

// Continuous: the bridge between two points on the live side stays there with probability
// 1 - exp(-2 d0 d1 / (sigma^2 dt)). A point on the dead side makes its gap zero through Max, so
// the factor is exactly zero without a branch, and the weight stays zero from then on.
template <class Real, class Model>
PathValue<Real> OutBridge(const Contract& c, const Model& model, std::span<const double> u) {
  const Direction direction = c.barrier.direction;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    const StepLaw<Real> law = model.Law(i, x);
    const Real next = law.mean + law.sd * InverseNormal(Real(u[i]));
    const Real variance = law.sd * law.sd;
    const Real d0 = Max(Gap(direction, x, log_barrier), Real(0.0));
    const Real d1 = Max(Gap(direction, next, log_barrier), Real(0.0));
    weight = weight * (1.0 - Exp(-2.0 * d0 * d1 / variance));
    x = next;
  }
  return Finish(c, model, x, weight);
}

// Continuous, hard: the same crossing probability, sampled against the step's second uniform,
// and the path killed outright. Unbiased, but its pathwise derivative ignores the barrier.
template <class Real, class Model>
PathValue<Real> OutSampledCrossing(const Contract& c, const Model& model, const PathUniforms& u) {
  const Direction direction = c.barrier.direction;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    const StepLaw<Real> law = model.Law(i, x);
    const Real next = law.mean + law.sd * InverseNormal(Real(u.normal[i]));
    const Real variance = law.sd * law.sd;
    const Real d0 = Max(Gap(direction, x, log_barrier), Real(0.0));
    const Real d1 = Max(Gap(direction, next, log_barrier), Real(0.0));
    const Real crossed = Exp(-2.0 * d0 * d1 / variance);
    weight = weight * Select(crossed <= u.crossing[i], Real(1.0), Real(0.0));
    x = next;
  }
  return Finish(c, model, x, weight);
}

// Discrete, one-step survival (Gerstner et al., Theorems 2.4 to 2.7). The step survives with
// p = Phi(a), a the standardised gap of the step's mean, the path is weighted by p, and the step is
// drawn from its law conditioned on the live side: for a down barrier z = -Phi^-1(p (1 - u)), as
// the reference draws it, and for an up barrier z = Phi^-1(p u). When p (1 - u) underflows to zero
// the path is dead (its weight is below 2^-1074 times the rest), the weight becomes zero and the
// draw uses p = 1, so the path stays finite. The reference would carry an infinity there.
template <class Real, class Model>
PathValue<Real> OutOneStep(const Contract& c, const Model& model, std::span<const double> u) {
  const bool down = c.barrier.direction == Direction::kDown;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    const StepLaw<Real> law = model.Law(i, x);
    const Real p = NormalCdf(Gap(c.barrier.direction, law.mean, log_barrier) / law.sd);
    const double v = down ? 1.0 - u[i] : u[i];
    const Real q = p * v;
    const auto alive = q > 0.0;
    weight = weight * Select(alive, p, Real(0.0));
    const Real conditional = InverseNormal(Select(alive, q, Real(v)));
    const Real z = down ? -conditional : conditional;
    x = law.mean + law.sd * z;
  }
  return Finish(c, model, x, weight);
}

// Discrete, hard indicator at the dates. The indicator carries no derivative, which is what makes
// the pathwise Greeks of this estimator wrong (the naive failure, NOTES.md).
template <class Real, class Model>
PathValue<Real> OutHard(const Contract& c, const Model& model, std::span<const double> u) {
  const bool down = c.barrier.direction == Direction::kDown;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    x = Advance(model, i, x, u[i]);
    const auto alive = down ? x > log_barrier : x < log_barrier;
    weight = weight * Select(alive, Real(1.0), Real(0.0));
  }
  return Finish(c, model, x, weight);
}

// Discrete, Savine's smooth barrier: at each date the path keeps the fraction of its notional
// that a linear ramp in spot from B - s (none) to B + s (all) gives, for a down barrier, and the
// mirror for an up barrier. s = width * S0 is a plain double, as in the source, so the width
// carries no derivative (Savine, section 12.4, printed p. 426).
template <class Real, class Model>
PathValue<Real> OutSmoothed(const Contract& c, const Model& model, std::span<const double> u) {
  const bool down = c.barrier.direction == Direction::kDown;
  const double s = c.barrier.smoothing_width * Value(model.Spot());
  const double lower = c.barrier.level - s;
  const double upper = c.barrier.level + s;
  const double width = 2.0 * s;
  Real x = model.LogSpot();
  Real weight(1.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    x = Advance(model, i, x, u[i]);
    const Real spot = Exp(x);
    const Real ramp = down ? (spot - lower) / width : (upper - spot) / width;
    weight = weight * Min(Max(ramp, Real(0.0)), Real(1.0));
  }
  return Finish(c, model, x, weight);
}

template <class Real, class Model>
PathValue<Real> KnockOut(const Contract& c, const Model& model, const PathUniforms& u) {
  switch (c.barrier.treatment) {
    case Treatment::kBridge:
      return OutBridge<Real>(c, model, u.normal);
    case Treatment::kOneStep:
      return OutOneStep<Real>(c, model, u.normal);
    case Treatment::kHard:
      return c.barrier.monitoring == Monitoring::kContinuous ? OutSampledCrossing<Real>(c, model, u)
                                                             : OutHard<Real>(c, model, u.normal);
    case Treatment::kSmoothed:
      return OutSmoothed<Real>(c, model, u.normal);
  }
  return {Real(0.0), Real(0.0)};
}

}  // namespace kernel_detail

// Precondition: Validate(c, market) passed, and u holds c.steps uniforms of each kind it reads.
template <class Real, class Model>
PathValue<Real> EvaluatePath(const Contract& c, const Model& model, const PathUniforms& u) {
  using namespace kernel_detail;
  switch (c.product) {
    case Product::kEuropean:
      return European<Real>(c, model, u.normal);
    case Product::kDigital:
      return Digital<Real>(c, model, u.normal);
    case Product::kAsianArithmetic:
      return AsianArithmetic<Real>(c, model, u.normal);
    case Product::kAsianGeometric:
      return AsianGeometric<Real>(c, model, u.normal);
    case Product::kBarrier: {
      const PathValue<Real> out = KnockOut<Real>(c, model, u);
      if (c.barrier.knock == Knock::kOut) return out;
      // Knock-in by parity on the same uniforms. Under one-step survival the knock-out leg runs
      // on its own conditioned path, so a single path's knock-in may be negative; its mean is not.
      const Real vanilla = European<Real>(c, model, u.normal).value;
      return {vanilla - out.value, out.weight};
    }
  }
  return {Real(0.0), Real(0.0)};
}

template <class Real, class Model>
Real DiscountedPayoff(const Contract& c, const Model& model, const PathUniforms& u) {
  return EvaluatePath<Real>(c, model, u).value;
}

}  // namespace aadp::pricer

#endif  // AADP_PRICER_KERNELS_HPP_
