// The local-volatility model behind model.hpp's Law seam, and the tally of its floored and
// capped local variances (docs/NOTES.md, "Stage 7a").
//
// Log-Euler with the local volatility frozen at the start of each step: over step s, from
// t_s = s dt to t_{s+1}, dt = T / steps,
//   x_{s+1} = x_s + (ln F(t_{s+1}) - ln F(t_s)) - v_s dt / 2 + sqrt(v_s dt) z_s,
//   v_s = sigma_loc^2(t_s, x_s - ln F(t_s)),
// with sigma_loc^2 from svi/local_vol.hpp after the floor rule. So Law returns that mean and
// sqrt(v_s dt), and every treatment uses the step's own law: one-step survival conditions on it,
// the bridge reads its frozen variance. With a constant local volatility the scheme is exact GBM
// with the curve's carry.
//
// What is active: only the surface's slice parameters, through svi::SviSurface<Real>. Times,
// segments, weights, ln F and dt are passive doubles fixed by the contract, built once here.
#ifndef AADP_PRICER_LOCAL_VOL_MODEL_HPP_
#define AADP_PRICER_LOCAL_VOL_MODEL_HPP_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "math/log.hpp"
#include "pricer/model.hpp"
#include "pricer/real_ops.hpp"
#include "svi/local_vol.hpp"
#include "svi/surface.hpp"

namespace aadp::pricer {

// Where the floored (or capped) points of one time segment lie.
struct FloorRange {
  std::uint64_t count = 0;
  double k_min = std::numeric_limits<double>::infinity();
  double k_max = -std::numeric_limits<double>::infinity();
  double t_min = std::numeric_limits<double>::infinity();
  double t_max = -std::numeric_limits<double>::infinity();

  void Add(double k, double t) {
    count += 1;
    k_min = std::min(k_min, k);
    k_max = std::max(k_max, k);
    t_min = std::min(t_min, t);
    t_max = std::max(t_max, t);
  }
  void Merge(const FloorRange& o) {
    count += o.count;
    k_min = std::min(k_min, o.k_min);
    k_max = std::max(k_max, o.k_max);
    t_min = std::min(t_min, o.t_min);
    t_max = std::max(t_max, o.t_max);
  }
};

// Every Euler step of every path, classified once. Counts and ranges are sums, minima and maxima,
// so merging the workers' tallies gives the same result for any thread count.
struct FloorTally {
  std::uint64_t paths = 0;
  std::uint64_t path_steps = 0;
  std::uint64_t paths_with_floor = 0;
  std::uint64_t paths_with_cap = 0;
  std::array<std::uint64_t, svi::kLocalVarianceClasses> by_class{};
  std::array<FloorRange, svi::kMaxSurfaceSlices> floored{};  // per time segment
  std::array<FloorRange, svi::kMaxSurfaceSlices> capped{};

  void Add(svi::LocalVarianceClass c, std::size_t segment, double k, double t) {
    path_steps += 1;
    by_class[static_cast<std::size_t>(c)] += 1;
    if (c == svi::LocalVarianceClass::kCapped) {
      capped[segment].Add(k, t);
    } else if (svi::IsFloored(c)) {
      floored[segment].Add(k, t);
    }
  }
  [[nodiscard]] std::uint64_t Floored() const { return Total(floored).count; }
  [[nodiscard]] std::uint64_t Capped() const { return Total(capped).count; }
  [[nodiscard]] static FloorRange Total(const std::array<FloorRange, svi::kMaxSurfaceSlices>& r) {
    FloorRange all;
    for (const FloorRange& x : r) all.Merge(x);
    return all;
  }
  void Merge(const FloorTally& o) {
    paths += o.paths;
    path_steps += o.path_steps;
    paths_with_floor += o.paths_with_floor;
    paths_with_cap += o.paths_with_cap;
    for (std::size_t i = 0; i < by_class.size(); ++i) by_class[i] += o.by_class[i];
    for (std::size_t i = 0; i < floored.size(); ++i) {
      floored[i].Merge(o.floored[i]);
      capped[i].Merge(o.capped[i]);
    }
  }
};

template <class Real>
class LocalVolModel {
 public:
  // Precondition: the surface validated, 0 < maturity <= its last expiry, steps >= 1.
  LocalVolModel(svi::SviSurface<Real> surface, double maturity, std::uint32_t steps,
                const svi::LocalVolRule& rule)
      : surface_(std::move(surface)),
        rule_(rule),
        dt_(maturity / static_cast<double>(steps)),
        spot_(surface_.Curve().Spot()),
        log_spot_(math::log(surface_.Curve().Spot())),
        discount_(surface_.Curve().Discount(maturity)) {
    const svi::ForwardCurve& curve = surface_.Curve();
    steps_.reserve(steps);
    double log_forward = curve.LogForward(0.0);
    for (std::uint32_t s = 0; s < steps; ++s) {
      const double t = static_cast<double>(s) * dt_;
      // The last step ends at the maturity itself, never past the last slice by a rounding.
      const double next = s + 1 == steps ? maturity : static_cast<double>(s + 1) * dt_;
      const double next_log_forward = curve.LogForward(next);
      steps_.push_back({.slot = surface_.Locate(t),
                        .time = t,
                        .log_forward = log_forward,
                        .log_forward_increment = next_log_forward - log_forward});
      log_forward = next_log_forward;
    }
  }

  // Counts every Law call into tally, or nothing when null. The model writes through the
  // pointer, so one model and one tally belong to one worker.
  void SetTally(FloorTally* tally) { tally_ = tally; }

  [[nodiscard]] const Real& Spot() const { return spot_; }
  [[nodiscard]] const Real& LogSpot() const { return log_spot_; }
  [[nodiscard]] const Real& Discount() const { return discount_; }
  [[nodiscard]] double StepLength() const { return dt_; }
  [[nodiscard]] const svi::SviSurface<Real>& Surface() const { return surface_; }

  [[nodiscard]] StepLaw<Real> Law(std::uint32_t step, const Real& x) const {
    const StepData& s = steps_[step];
    const Real k = x - s.log_forward;
    const svi::LocalVariance<Real> lv = svi::LocalVarianceAt(surface_.At(s.slot, k), k, rule_);
    if (tally_ != nullptr) tally_->Add(lv.cls, s.slot.segment, Value(k), s.time);
    const Real variance = lv.variance * dt_;
    return {(x + s.log_forward_increment) - 0.5 * variance, Sqrt(variance)};
  }

 private:
  struct StepData {
    svi::TimeSlot slot;
    double time = 0.0;
    double log_forward = 0.0;
    double log_forward_increment = 0.0;
  };

  svi::SviSurface<Real> surface_;
  svi::LocalVolRule rule_;
  double dt_;
  Real spot_;
  Real log_spot_;
  Real discount_;
  std::vector<StepData> steps_;
  FloorTally* tally_ = nullptr;
};

}  // namespace aadp::pricer

#endif  // AADP_PRICER_LOCAL_VOL_MODEL_HPP_
