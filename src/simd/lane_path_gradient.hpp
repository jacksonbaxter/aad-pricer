// Pathwise gradients on lanes: ad::PathGradient for a group of N paths on the vector tape
// (lane_tape.hpp), and the RunLaneBatches path function built on it.
//
// How a worker spends its tapes. It holds an ad::PathGradient, whose scalar tape records the
// set-up once, exactly as the scalar gradient does, and a LaneTape whose indices start at that
// tape's mark, so lane operands name the set-up by its scalar indices. A group of N paths is
// recorded on the lane tape after its mark, swept back to it, and rewound. The contributions the
// sweep makes to the set-up wait in the boundary buffer and are then added into the scalar tape's
// set-up adjoints lane by lane, in path order, for the lanes inside the block: the same additions
// in the same order as the scalar tape makes them path after path. At the end of each block the
// scalar tape's own SweepSetup runs, once per block, as in ad::GradientPathFn. So the block sums,
// and every Greek, are the scalar gradient's bits.
//
// Fallback. A group whose Selects diverge across lanes (lane_tape_ops.hpp) is rewound, and its
// lanes inside the block run on the scalar tape, one path at a time in path order, into the same
// set-up adjoints. Each fallback is counted and returned (LaneGradientEstimate). The pricer's
// kernels have no such Select, so for them the count is zero; tests/simd/lane_gradient_test.cpp
// forces it with a kernel of its own.
//
// A Kernel gives the payoff both ways: Lanes(contract, model, LaneUniforms<LaneTapeReal<D>>) on the
// lane tape and Scalar(contract, GbmModel<ad::Real>, PathUniforms) on the scalar one, the same
// operations in the same order. PricerKernel is the pricer's.
#ifndef AADP_SIMD_LANE_PATH_GRADIENT_HPP_
#define AADP_SIMD_LANE_PATH_GRADIENT_HPP_

#include <hwy/highway.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ad/gradient.hpp"
#include "ad/real.hpp"
#include "ad/tape.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/runner.hpp"
#include "simd/lane_gradient.hpp"
#include "simd/lane_kernels.hpp"
#include "simd/lane_real.hpp"
#include "simd/lane_runner.hpp"
#include "simd/lane_tape.hpp"
#include "simd/lane_tape_ops.hpp"

HWY_BEFORE_NAMESPACE();

namespace aadp::simd {

namespace hn = hwy::HWY_NAMESPACE;

// The scalar model's five set-up Reals, broadcast, with LaneGbm's interface. The set-up lives on
// the scalar tape, so a lane never records it.
template <class D>
class LaneTapeGbm {
 public:
  using Real = LaneTapeReal<D>;

  explicit LaneTapeGbm(const pricer::GbmModel<ad::Real>& m)
      : spot_(Real::FromScalar(m.Spot())),
        log_spot_(Real::FromScalar(m.LogSpot())),
        discount_(Real::FromScalar(m.Discount())),
        drift_(Real::FromScalar(m.Drift())),
        sd_(Real::FromScalar(m.StepSd())),
        spot_value_(m.Spot().value()) {}

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

// The pricer's discounted payoff, on lanes and on the scalar tape.
struct PricerKernel {
  template <class Real, class Model>
  [[nodiscard]] Real Lanes(const pricer::Contract& c, const Model& model,
                           const LaneUniforms<Real>& u) const {
    return EvaluateLanes<Real>(c, model, u).value;
  }
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static): a Kernel may carry state.
  [[nodiscard]] ad::Real Scalar(const pricer::Contract& c, const pricer::GbmModel<ad::Real>& model,
                                const pricer::PathUniforms& u) const {
    return pricer::DiscountedPayoff<ad::Real>(c, model, u);
  }
};

template <class D, class Kernel = PricerKernel>
class LanePathGradient {
 public:
  static constexpr std::size_t kLanes = hn::MaxLanes(D());
  using Real = LaneTapeReal<D>;
  using Uniforms = LaneUniforms<LaneReal<D>>;

  // Precondition: Validate(contract, market) passed. Activates a scalar tape and a lane tape on
  // the constructing thread, so it must be built, used and destroyed there.
  LanePathGradient(const pricer::Contract& contract, const pricer::Market<double>& market,
                   const Kernel& kernel = {})
      : contract_(contract),
        kernel_(kernel),
        scalar_(contract, market),
        tape_(static_cast<ad::Index>(scalar_.mark().statements)),
        scope_(tape_),
        model_(scalar_.model()),
        mark_(tape_.GetPosition()),
        normal_(contract.steps),
        crossing_(pricer::NeedsCrossingUniforms(contract) ? contract.steps : 0),
        scalar_normal_(contract.steps),
        scalar_crossing_(crossing_.size()),
        lane_values_(contract.steps * kLanes) {}

  // Records the group (lane i is path first + i), sweeps it, and adds the set-up contributions of
  // lanes 0..count-1 into the scalar tape's set-up adjoints in path order. Returns the payoffs of
  // every lane and whether the group fell back to the scalar tape.
  struct GroupValue {
    hn::Vec<D> payoff;
    bool fell_back = false;
  };
  GroupValue Accumulate(const Uniforms& u, std::size_t count) {
    const Real payoff = Record(u);
    if (tape_.Divergent()) return {.payoff = FallBack(u, count), .fell_back = true};
    tape_.AddAdjoint(payoff, hn::Set(D(), 1.0));
    tape_.SweepBack(tape_.GetPosition(), mark_);
    for (std::size_t lane = 0; lane < count; ++lane) Replay(lane);
    tape_.ClearBoundary();
    tape_.Rewind(mark_);
    return {.payoff = payoff.vec(), .fell_back = false};
  }
  // The set-up sweep: dV/d(input) summed over every path accumulated since the last call.
  [[nodiscard]] std::array<double, ad::kInputs> SweepSetup() { return scalar_.SweepSetup(); }

  // Every lane of one group alone, for checks: its value and its gradient after its own set-up
  // sweep. Starts and ends with the set-up adjoints clear.
  struct GroupResult {
    std::array<double, kLanes> value{};
    std::array<std::array<double, ad::kInputs>, kLanes> gradient{};
    bool fell_back = false;
  };
  [[nodiscard]] GroupResult Evaluate(const Uniforms& u) {
    GroupResult result;
    const Real payoff = Record(u);
    if (tape_.Divergent()) {
      ClearGroup();
      result.fell_back = true;
      for (std::size_t lane = 0; lane < kLanes; ++lane) {
        const ad::Real scalar_payoff = ScalarPath(u, lane);
        result.value[lane] = scalar_payoff.value();
        result.gradient[lane] = scalar_.SweepSetup();
      }
      return result;
    }
    hn::StoreU(payoff.vec(), D(), result.value.data());
    tape_.AddAdjoint(payoff, hn::Set(D(), 1.0));
    tape_.SweepBack(tape_.GetPosition(), mark_);
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      Replay(lane);
      result.gradient[lane] = scalar_.SweepSetup();
    }
    tape_.ClearBoundary();
    tape_.Rewind(mark_);
    return result;
  }

  // Records the group after the mark and leaves it there, with any divergence marked. The caller
  // sweeps or reads it, then calls ClearGroup. Precondition, here and above: u holds
  // contract.steps uniforms of each kind the contract reads.
  [[nodiscard]] Real Record(const Uniforms& u) {
    // The kernel reads steps uniforms of each kind it needs, and no more.
    for (std::size_t i = 0; i < normal_.size(); ++i) normal_[i] = Real(u.normal[i].vec());
    for (std::size_t i = 0; i < crossing_.size(); ++i) crossing_[i] = Real(u.crossing[i].vec());
    const LaneUniforms<Real> lanes{.normal = normal_, .crossing = crossing_};
    return kernel_.Lanes(contract_, model_, lanes);
  }
  void ClearGroup() {
    tape_.ClearDivergence();
    tape_.ClearBoundary();
    tape_.Rewind(mark_);
  }
  // Adds lane's boundary contributions, in sweep order, into the scalar tape's set-up adjoints.
  void Replay(std::size_t lane) {
    ad::Tape& scalar = scalar_.tape();
    tape_.ForEachBoundary(lane, [&](ad::Index index, double c) { scalar.AddAdjointAt(index, c); });
  }

  [[nodiscard]] LaneTape<D>& tape() { return tape_; }
  [[nodiscard]] ad::PathGradient& scalar() { return scalar_; }
  [[nodiscard]] ad::Position mark() const { return mark_; }
  [[nodiscard]] std::uint64_t fallback_groups() const { return fallback_groups_; }
  [[nodiscard]] std::uint64_t fallback_paths() const { return fallback_paths_; }

 private:
  // Lane `lane` of the group on the scalar tape: recorded after its mark, swept back to it and
  // rewound, as ad::PathGradient::Accumulate does, with the kernel's scalar form.
  ad::Real ScalarPath(const Uniforms& u, std::size_t lane) {
    for (std::size_t i = 0; i < scalar_normal_.size(); ++i) {
      hn::StoreU(u.normal[i].vec(), D(), lane_values_.data() + (i * kLanes));
      scalar_normal_[i] = lane_values_[(i * kLanes) + lane];
    }
    for (std::size_t i = 0; i < scalar_crossing_.size(); ++i) {
      hn::StoreU(u.crossing[i].vec(), D(), lane_values_.data() + (i * kLanes));
      scalar_crossing_[i] = lane_values_[(i * kLanes) + lane];
    }
    const pricer::PathUniforms path{.normal = scalar_normal_, .crossing = scalar_crossing_};
    ad::Tape& scalar = scalar_.tape();
    const ad::Real payoff = kernel_.Scalar(contract_, scalar_.model(), path);
    scalar.AddAdjoint(payoff, 1.0);
    scalar.SweepBack(scalar.GetPosition(), scalar_.mark());
    scalar_.Rewind();
    return payoff;
  }

  hn::Vec<D> FallBack(const Uniforms& u, std::size_t count) {
    ClearGroup();
    std::array<double, kLanes> payoffs{};
    for (std::size_t lane = 0; lane < count; ++lane) payoffs[lane] = ScalarPath(u, lane).value();
    fallback_groups_ += 1;
    fallback_paths_ += count;
    return hn::LoadU(D(), payoffs.data());
  }

  pricer::Contract contract_;
  Kernel kernel_;
  ad::PathGradient scalar_;
  LaneTape<D> tape_;
  LaneTapeScope<D> scope_;
  LaneTapeGbm<D> model_;
  ad::Position mark_;
  std::vector<Real> normal_;
  std::vector<Real> crossing_;
  std::vector<double> scalar_normal_;
  std::vector<double> scalar_crossing_;
  std::vector<double> lane_values_;
  std::uint64_t fallback_groups_ = 0;
  std::uint64_t fallback_paths_ = 0;
};

// Fallbacks over a whole run, from every worker.
struct LaneFallbacks {
  std::atomic<std::uint64_t> groups{0};
  std::atomic<std::uint64_t> paths{0};
};

// The RunLaneBatches path function: statistic 0 is the payoff, 1 to 5 the Greeks, which only
// EndBlock fills, as in ad::GradientPathFn.
template <class D, class Kernel = PricerKernel>
class LaneGradientFn {
 public:
  LaneGradientFn(const pricer::Contract& contract, const pricer::Market<double>& market,
                 const Kernel& kernel, LaneFallbacks& fallbacks)
      : path_(contract, market, kernel), fallbacks_(&fallbacks) {}

  void operator()(const LaneUniforms<LaneReal<D>>& u,
                  std::array<LaneReal<D>, ad::kGradientStats>& stats, std::size_t count) {
    stats = {};
    const auto group = path_.Accumulate(u, count);
    stats[0] = LaneReal<D>(group.payoff);
    if (group.fell_back) {
      fallbacks_->groups.fetch_add(1, std::memory_order_relaxed);
      fallbacks_->paths.fetch_add(count, std::memory_order_relaxed);
    }
  }
  void EndBlock(std::array<double, ad::kGradientStats>& sum) {
    const std::array<double, ad::kInputs> gradient = path_.SweepSetup();
    for (std::size_t i = 0; i < ad::kInputs; ++i) {
      sum[1 + i] += i == ad::kMaturityInput ? -gradient[i] : gradient[i];
    }
  }

 private:
  LanePathGradient<D, Kernel> path_;
  LaneFallbacks* fallbacks_;
};

// PriceWithGradient on lanes of tag D. Precondition: Validate and ValidateRun passed.
template <class D, class Kernel = PricerKernel>
LaneGradientEstimate RunLaneGradient(D d, const pricer::Contract& contract,
                                     const pricer::Market<double>& market,
                                     const pricer::RunConfig& config, const Kernel& kernel = {}) {
  LaneFallbacks fallbacks;
  const pricer::PathShape shape{.steps = contract.steps,
                                .crossing = pricer::NeedsCrossingUniforms(contract)};
  const auto table = RunLaneBatches<ad::kGradientStats>(d, config, shape, [&] {
    return LaneGradientFn<D, Kernel>(contract, market, kernel, fallbacks);
  });
  return {.estimate = ad::EstimateFromTable(table),
          .fallback_groups = fallbacks.groups.load(),
          .fallback_paths = fallbacks.paths.load()};
}

}  // namespace aadp::simd

HWY_AFTER_NAMESPACE();

#endif  // AADP_SIMD_LANE_PATH_GRADIENT_HPP_
