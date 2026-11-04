// The path kernels of src/pricer/kernels.hpp for a Real whose lanes are paths.
//
// Each kernel is its twin in kernels.hpp with one change: a uniform is a Real, not a double, since
// each lane has its own path. Every operation and its order is the same, so with LaneReal
// (lane_real.hpp) lane i gives the bits EvaluatePath<double> gives path i, and with Real = double
// these are the scalar kernels again. tests/simd/path_agreement_test.cpp checks both.
//
// All of them are branch-free, so every product and treatment runs on lanes: European, digital,
// both Asians, the bridge, the sampled continuous crossing, one-step survival, the hard discrete
// indicator, smoothing, and knock-ins by parity. The hard indicators are Selects of 1 and 0, and
// the one-step guard, a `?:` on Value(q) in the scalar kernel, is a Select here; for the primal
// both are the identity. The only branches are on the contract, the same for every lane.
//
// The model is LaneGbm below: GbmModel<double>'s five numbers, broadcast. So the set-up is the
// scalar model's own bits, and a lane never recomputes it.
#ifndef AADP_SIMD_LANE_KERNELS_HPP_
#define AADP_SIMD_LANE_KERNELS_HPP_

#include <cstdint>
#include <span>

#include "pricer/contract.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/real_ops.hpp"
#include "simd/lane_real.hpp"

namespace aadp::simd {

// What a lane kernel reads: one normal uniform per step, and one crossing uniform per step for the
// contracts that sample a crossing (empty otherwise).
template <class Real>
struct LaneUniforms {
  std::span<const Real> normal;
  std::span<const Real> crossing;
};

// GbmModel<double> broadcast to Real, with the same interface, and the spot as a double for the
// smoothing width, which carries no derivative.
template <class Real>
class LaneGbm {
 public:
  explicit LaneGbm(const pricer::GbmModel<double>& m)
      : spot_(m.Spot()),
        log_spot_(m.LogSpot()),
        discount_(m.Discount()),
        drift_(m.Drift()),
        sd_(m.StepSd()),
        spot_value_(m.Spot()) {}

  [[nodiscard]] const Real& Spot() const { return spot_; }
  [[nodiscard]] const Real& LogSpot() const { return log_spot_; }
  [[nodiscard]] const Real& Discount() const { return discount_; }
  [[nodiscard]] double SpotValue() const { return spot_value_; }
  [[nodiscard]] pricer::StepLaw<Real> Law(std::uint32_t /*step*/, const Real& x) const {
    return {x + drift_, sd_};
  }

 private:
  Real spot_;
  Real log_spot_;
  Real discount_;
  Real drift_;
  Real sd_;
  double spot_value_;
};

namespace lane_detail {

// For Real = double, the scalar operations; LaneReal finds its own by argument-dependent lookup.
using pricer::Contract;
using pricer::Direction;
using pricer::Exp;
using pricer::InverseNormal;
using pricer::Knock;
using pricer::Log;
using pricer::Max;
using pricer::Min;
using pricer::Monitoring;
using pricer::NormalCdf;
using pricer::OptionType;
using pricer::PathValue;
using pricer::Product;
using pricer::Select;
using pricer::StepLaw;
using pricer::Treatment;
using pricer::Value;

template <class Real>
Real Intrinsic(OptionType type, const Real& underlying, double strike) {
  const Real intrinsic = type == OptionType::kCall ? underlying - strike : strike - underlying;
  return Select(intrinsic > 0.0, intrinsic, Real(0.0));
}

template <class Real, class Model>
Real Advance(const Model& model, std::uint32_t step, const Real& x, const Real& uniform) {
  const StepLaw<Real> law = model.Law(step, x);
  return law.mean + law.sd * InverseNormal(uniform);
}

template <class Real, class Model>
PathValue<Real> European(const Contract& c, const Model& model, std::span<const Real> u) {
  Real x = model.LogSpot();
  for (std::uint32_t i = 0; i < c.steps; ++i) x = Advance(model, i, x, u[i]);
  return {model.Discount() * Intrinsic(c.type, Real(Exp(x)), c.strike), Real(1.0)};
}

template <class Real, class Model>
PathValue<Real> Digital(const Contract& c, const Model& model, std::span<const Real> u) {
  Real x = model.LogSpot();
  for (std::uint32_t i = 0; i + 1 < c.steps; ++i) x = Advance(model, i, x, u[i]);
  const StepLaw<Real> law = model.Law(c.steps - 1, x);
  const Real d = (law.mean - Log(Real(c.strike))) / law.sd;
  const Real p = NormalCdf(c.type == OptionType::kCall ? d : -d);
  return {model.Discount() * p, Real(1.0)};
}

template <class Real, class Model>
PathValue<Real> AsianArithmetic(const Contract& c, const Model& model, std::span<const Real> u) {
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
PathValue<Real> AsianGeometric(const Contract& c, const Model& model, std::span<const Real> u) {
  Real x = model.LogSpot();
  Real sum = c.include_spot ? x : Real(0.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    x = Advance(model, i, x, u[i]);
    sum = sum + x;
  }
  const double count = static_cast<double>(c.steps) + (c.include_spot ? 1.0 : 0.0);
  return {model.Discount() * Intrinsic(c.type, Real(Exp(sum / count)), c.strike), Real(1.0)};
}

template <class Real>
Real Gap(Direction direction, const Real& x, const Real& log_barrier) {
  return direction == Direction::kDown ? x - log_barrier : log_barrier - x;
}

template <class Real, class Model>
PathValue<Real> Finish(const Contract& c, const Model& model, const Real& x, const Real& weight) {
  return {model.Discount() * weight * Intrinsic(c.type, Real(Exp(x)), c.strike), weight};
}

template <class Real, class Model>
PathValue<Real> OutBridge(const Contract& c, const Model& model, std::span<const Real> u) {
  const Direction direction = c.barrier.direction;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    const StepLaw<Real> law = model.Law(i, x);
    const Real next = law.mean + law.sd * InverseNormal(u[i]);
    const Real variance = law.sd * law.sd;
    const Real d0 = Max(Gap(direction, x, log_barrier), Real(0.0));
    const Real d1 = Max(Gap(direction, next, log_barrier), Real(0.0));
    weight = weight * (1.0 - Exp(-2.0 * d0 * d1 / variance));
    x = next;
  }
  return Finish(c, model, x, weight);
}

template <class Real, class Model>
PathValue<Real> OutSampledCrossing(const Contract& c, const Model& model,
                                   const LaneUniforms<Real>& u) {
  const Direction direction = c.barrier.direction;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    const StepLaw<Real> law = model.Law(i, x);
    const Real next = law.mean + law.sd * InverseNormal(u.normal[i]);
    const Real variance = law.sd * law.sd;
    const Real d0 = Max(Gap(direction, x, log_barrier), Real(0.0));
    const Real d1 = Max(Gap(direction, next, log_barrier), Real(0.0));
    const Real crossed = Exp(-2.0 * d0 * d1 / variance);
    weight = weight * Select(crossed <= u.crossing[i], Real(1.0), Real(0.0));
    x = next;
  }
  return Finish(c, model, x, weight);
}

// The guard below 2^-1000 is kernels.hpp's (its comment gives the reach and the bias), as a
// Select, so every lane runs it.
template <class Real, class Model>
PathValue<Real> OutOneStep(const Contract& c, const Model& model, std::span<const Real> u) {
  const bool down = c.barrier.direction == Direction::kDown;
  const Real log_barrier = Log(Real(c.barrier.level));
  Real x = model.LogSpot();
  Real weight(1.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    const StepLaw<Real> law = model.Law(i, x);
    const Real p = NormalCdf(Gap(c.barrier.direction, law.mean, log_barrier) / law.sd);
    const Real v = down ? 1.0 - u[i] : u[i];
    const Real q = p * v;
    const auto alive = q > 0.0;
    weight = weight * Select(alive, p, Real(0.0));
    const Real drawn = Select(Value(q) < 0x1p-1000, Real(Value(q)), q);
    const Real conditional = InverseNormal(Select(alive, drawn, v));
    const Real z = down ? -conditional : conditional;
    x = law.mean + law.sd * z;
  }
  return Finish(c, model, x, weight);
}

template <class Real, class Model>
PathValue<Real> OutHard(const Contract& c, const Model& model, std::span<const Real> u) {
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

template <class Real, class Model>
PathValue<Real> OutSmoothed(const Contract& c, const Model& model, std::span<const Real> u) {
  const bool down = c.barrier.direction == Direction::kDown;
  const double s = c.barrier.smoothing_width * model.SpotValue();
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
PathValue<Real> KnockOut(const Contract& c, const Model& model, const LaneUniforms<Real>& u) {
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

}  // namespace lane_detail

// pricer::EvaluatePath with a Real per lane. Precondition: Validate(c, market) passed, and u
// holds c.steps uniforms of each kind it reads.
template <class Real, class Model>
pricer::PathValue<Real> EvaluateLanes(const pricer::Contract& c, const Model& model,
                                      const LaneUniforms<Real>& u) {
  using namespace lane_detail;
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
      const Real vanilla = European<Real>(c, model, u.normal).value;
      return {vanilla - out.value, out.weight};
    }
  }
  return {Real(0.0), Real(0.0)};
}

}  // namespace aadp::simd

#endif  // AADP_SIMD_LANE_KERNELS_HPP_
