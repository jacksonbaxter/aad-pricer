// Path kernels with n Euler sub-steps between the contract's dates, for the local-volatility model
// (docs/NOTES.md, "Stage 7a"). The contract keeps its m dates t_i = i T / m: an Asian fixes and a
// discrete barrier monitors there only, while a continuous barrier is bridged on every sub-step.
// The model sees m n steps of T / (m n), step s using the uniforms of step s.
//
// A separate header, so kernels.hpp, and with it the lanes, the second-order code and the frozen
// path checks, is unchanged. At n = 1 every kernel here performs kernels.hpp's operations in its
// order, so on the same model it gives EvaluatePath's bits (tests/pricer/local_vol_test.cpp).
// The same branch-free rule holds: no branch on a Real.
#ifndef AADP_PRICER_SUBSTEP_KERNELS_HPP_
#define AADP_PRICER_SUBSTEP_KERNELS_HPP_

#include <cstdint>
#include <span>

#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/real_ops.hpp"

namespace aadp::pricer {

namespace substep_detail {

using kernel_detail::Advance;
using kernel_detail::Finish;
using kernel_detail::Gap;
using kernel_detail::Intrinsic;

template <class Real, class Model>
PathValue<Real> European(const Contract& c, std::uint32_t n, const Model& model,
                         std::span<const double> u) {
  Real x = model.LogSpot();
  const std::uint32_t steps = c.steps * n;
  for (std::uint32_t s = 0; s < steps; ++s) x = Advance(model, s, x, u[s]);
  return {model.Discount() * Intrinsic(c.type, Real(Exp(x)), c.strike), Real(1.0)};
}

template <class Real, class Model>
PathValue<Real> AsianArithmetic(const Contract& c, std::uint32_t n, const Model& model,
                                std::span<const double> u) {
  Real x = model.LogSpot();
  Real sum = c.include_spot ? model.Spot() : Real(0.0);
  std::uint32_t s = 0;
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    for (std::uint32_t j = 0; j < n; ++j, ++s) x = Advance(model, s, x, u[s]);
    sum = sum + Exp(x);
  }
  const double count = static_cast<double>(c.steps) + (c.include_spot ? 1.0 : 0.0);
  return {model.Discount() * Intrinsic(c.type, Real(sum / count), c.strike), Real(1.0)};
}

// Continuous, bridge on every sub-step with that step's variance.
template <class Real, class Model>
PathValue<Real> OutBridge(const Contract& c, std::uint32_t n, const Model& model,
                          std::span<const double> u) {
  const Direction direction = c.barrier.direction;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  const std::uint32_t steps = c.steps * n;
  for (std::uint32_t s = 0; s < steps; ++s) {
    const StepLaw<Real> law = model.Law(s, x);
    const Real next = law.mean + law.sd * InverseNormal(Real(u[s]));
    const Real variance = law.sd * law.sd;
    const Real d0 = Max(Gap(direction, x, log_barrier), Real(0.0));
    const Real d1 = Max(Gap(direction, next, log_barrier), Real(0.0));
    weight = weight * (1.0 - Exp(-2.0 * d0 * d1 / variance));
    x = next;
  }
  return Finish(c, model, x, weight);
}

// Continuous, a sampled crossing on every sub-step against its second uniform.
template <class Real, class Model>
PathValue<Real> OutSampledCrossing(const Contract& c, std::uint32_t n, const Model& model,
                                   const PathUniforms& u) {
  const Direction direction = c.barrier.direction;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  const std::uint32_t steps = c.steps * n;
  for (std::uint32_t s = 0; s < steps; ++s) {
    const StepLaw<Real> law = model.Law(s, x);
    const Real next = law.mean + law.sd * InverseNormal(Real(u.normal[s]));
    const Real variance = law.sd * law.sd;
    const Real d0 = Max(Gap(direction, x, log_barrier), Real(0.0));
    const Real d1 = Max(Gap(direction, next, log_barrier), Real(0.0));
    const Real crossed = Exp(-2.0 * d0 * d1 / variance);
    weight = weight * Select(crossed <= u.crossing[s], Real(1.0), Real(0.0));
    x = next;
  }
  return Finish(c, model, x, weight);
}

// Discrete, one-step survival on the sub-step that ends at each date, with that step's law; the
// other sub-steps are plain. The conditioned step is kernels.hpp's OutOneStep step, guard and all.
template <class Real, class Model>
PathValue<Real> OutOneStep(const Contract& c, std::uint32_t n, const Model& model,
                           std::span<const double> u) {
  const bool down = c.barrier.direction == Direction::kDown;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  std::uint32_t s = 0;
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    for (std::uint32_t j = 0; j + 1 < n; ++j, ++s) x = Advance(model, s, x, u[s]);
    const StepLaw<Real> law = model.Law(s, x);
    const Real p = NormalCdf(Gap(c.barrier.direction, law.mean, log_barrier) / law.sd);
    const double v = down ? 1.0 - u[s] : u[s];
    const Real q = p * v;
    const auto alive = q > 0.0;
    weight = weight * Select(alive, p, Real(0.0));
    // kernels.hpp, OutOneStep: below 2^-1000 the draw carries no derivative.
    const Real drawn = Value(q) < 0x1p-1000 ? Real(Value(q)) : q;
    const Real conditional = InverseNormal(Select(alive, drawn, Real(v)));
    const Real z = down ? -conditional : conditional;
    x = law.mean + law.sd * z;
    ++s;
  }
  return Finish(c, model, x, weight);
}

// Discrete, hard indicator at the dates.
template <class Real, class Model>
PathValue<Real> OutHard(const Contract& c, std::uint32_t n, const Model& model,
                        std::span<const double> u) {
  const bool down = c.barrier.direction == Direction::kDown;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  std::uint32_t s = 0;
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    for (std::uint32_t j = 0; j < n; ++j, ++s) x = Advance(model, s, x, u[s]);
    const auto alive = down ? x > log_barrier : x < log_barrier;
    weight = weight * Select(alive, Real(1.0), Real(0.0));
  }
  return Finish(c, model, x, weight);
}

// Discrete, Savine's ramp at the dates, s = width * S0 a plain double.
template <class Real, class Model>
PathValue<Real> OutSmoothed(const Contract& c, std::uint32_t n, const Model& model,
                            std::span<const double> u) {
  const bool down = c.barrier.direction == Direction::kDown;
  const double half = c.barrier.smoothing_width * Value(model.Spot());
  const double lower = c.barrier.level - half;
  const double upper = c.barrier.level + half;
  const double width = 2.0 * half;
  Real x = model.LogSpot();
  Real weight(1.0);
  std::uint32_t s = 0;
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    for (std::uint32_t j = 0; j < n; ++j, ++s) x = Advance(model, s, x, u[s]);
    const Real spot = Exp(x);
    const Real ramp = down ? (spot - lower) / width : (upper - spot) / width;
    weight = weight * Min(Max(ramp, Real(0.0)), Real(1.0));
  }
  return Finish(c, model, x, weight);
}

}  // namespace substep_detail

// The products and treatments the local-volatility pricer accepts: European, arithmetic Asian
// and knock-out barriers under every treatment the GBM pricer has.
constexpr bool SubstepSupports(const Contract& c) {
  switch (c.product) {
    case Product::kEuropean:
    case Product::kAsianArithmetic:
      return true;
    case Product::kBarrier:
      return c.barrier.knock == Knock::kOut;
    case Product::kDigital:
    case Product::kAsianGeometric:
      return false;
  }
  return false;
}

// Precondition: Validate passed for the contract, SubstepSupports(c), n >= 1, and u holds
// c.steps * n uniforms of each kind the contract reads.
template <class Real, class Model>
PathValue<Real> EvaluateSubstepped(const Contract& c, std::uint32_t n, const Model& model,
                                   const PathUniforms& u) {
  using namespace substep_detail;
  if (c.product == Product::kEuropean) return European<Real>(c, n, model, u.normal);
  if (c.product == Product::kAsianArithmetic) return AsianArithmetic<Real>(c, n, model, u.normal);
  switch (c.barrier.treatment) {
    case Treatment::kBridge:
      return OutBridge<Real>(c, n, model, u.normal);
    case Treatment::kOneStep:
      return OutOneStep<Real>(c, n, model, u.normal);
    case Treatment::kHard:
      return c.barrier.monitoring == Monitoring::kContinuous
                 ? OutSampledCrossing<Real>(c, n, model, u)
                 : OutHard<Real>(c, n, model, u.normal);
    case Treatment::kSmoothed:
      return OutSmoothed<Real>(c, n, model, u.normal);
  }
  return {Real(0.0), Real(0.0)};
}

}  // namespace aadp::pricer

#endif  // AADP_PRICER_SUBSTEP_KERNELS_HPP_
