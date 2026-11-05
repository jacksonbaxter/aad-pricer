// Raw SVI slices joined in time, with the forward and discount curves that come with them, as
// NOTES.md, "Stage 7a", pre-registers. Templated on Real, so that the 15 slice parameters can be
// the tape's inputs; the times, forwards and discounts are passive doubles.
//
// Time. Slices j = 0..n-1 at expiries T_0 < ... < T_{n-1}. Total variance is linear in t at fixed
// k between adjacent slices, and (t / T_0) w_0(k) before the first:
//   segment 0     0 <= t < T_0          w = lambda w_0,                  lambda = t / T_0
//   segment j     T_{j-1} <= t < T_j    w = (1 - alpha) w_{j-1} + alpha w_j,
//                                       alpha = (t - T_{j-1}) / (T_j - T_{j-1})
// with w' and w'' in k the same, so dT w is w_0 / T_0, or (w_j - w_{j-1}) / (T_j - T_{j-1}). By
// Gatheral and Jacquier, arXiv 1204.0646, Lemma 2.1, the sign of dT w is then exactly the
// calendar check between the two slices at that k. Nothing is extrapolated past T_{n-1}: callers
// refuse such a time, and Locate's precondition excludes it.
//
// Curves. S0 = D_0 F_0, the nearest parity forward discounted to today (no dividend before
// T_0). ln F(t) is linear in t between the nodes (0, ln S0) and (T_j, ln F_j), ln D(t) between
// (0, 0) and (T_j, ln D_j), so carry and rate are piecewise constant, and carry comes from F.
//
// A Real needs what raw_svi.hpp asks, plus multiplication by a double.
#ifndef AADP_SVI_SURFACE_HPP_
#define AADP_SVI_SURFACE_HPP_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "svi/raw_svi.hpp"

namespace aadp::svi {

// The plain value, for the floor rule's decisions; the tape's and the Dual's come by ADL.
inline double Value(double x) { return x; }

// Validation limits. Inside them every value and every recorded partial of the surface and of
// local_vol.hpp is finite (NOTES.md, "Stage 7a", floor rule). Total variance of 1e4 is an implied
// volatility of 100 at T = 1, and 1e-6 years is 32 seconds.
inline constexpr std::size_t kMaxSurfaceSlices = 8;
inline constexpr double kMinSurfaceTime = 1e-6;  // the first expiry, and each gap between two
inline constexpr double kMaxSurfaceExpiry = 100.0;
inline constexpr double kMaxSurfaceParameter = 1e4;  // |a|, b, |m| and sigma
inline constexpr double kMinSurfaceSigma = 1e-8;
inline constexpr double kMinCurveLevel = 1e-8;  // forwards and discounts
inline constexpr double kMaxCurveLevel = 1e8;

struct SliceTerms {
  double expiry = 0.0;    // T_j in years
  double forward = 0.0;   // F_j
  double discount = 1.0;  // D_j
};

enum class SurfaceError : std::uint8_t {
  kNone,
  kSliceCount,  // none, more than kMaxSurfaceSlices, or terms and parameters of different sizes
  kExpiry,      // not finite, below kMinSurfaceTime, above the maximum, or a gap below it
  kForward,
  kDiscount,
  kParameters,  // not finite, b < 0, |rho| > 1, sigma below kMinSurfaceSigma, or out of range
};

[[nodiscard]] const char* SurfaceErrorName(SurfaceError error) noexcept;

[[nodiscard]] SurfaceError ValidateSurface(std::span<const SliceTerms> terms,
                                           std::span<const RawSvi<double>> params);

// The forward and discount curves. Precondition: ValidateSurface passed. Times in [0, T_{n-1}].
class ForwardCurve {
 public:
  explicit ForwardCurve(std::span<const SliceTerms> terms);

  [[nodiscard]] double Spot() const { return spot_; }
  [[nodiscard]] double LastExpiry() const { return times_.back(); }
  [[nodiscard]] double LogForward(double t) const { return Interpolate(log_forward_, t); }
  [[nodiscard]] double LogDiscount(double t) const { return Interpolate(log_discount_, t); }
  [[nodiscard]] double Discount(double t) const;

 private:
  [[nodiscard]] double Interpolate(const std::vector<double>& values, double t) const;

  double spot_ = 0.0;
  std::vector<double> times_;  // 0, T_0, ..., T_{n-1}
  std::vector<double> log_forward_;
  std::vector<double> log_discount_;
};

// Where t sits: its segment, as above, its weight (lambda in segment 0, alpha after), and the
// segment's 1 / length.
struct TimeSlot {
  std::size_t segment = 0;
  double weight = 0.0;
  double inverse_length = 0.0;
};

// Precondition: expiries strictly increasing, 0 <= t <= the last. t = T_{n-1} is the end of the
// last segment, alpha = 1; every other slice time starts its segment, alpha = 0.
[[nodiscard]] TimeSlot Locate(std::span<const double> expiries, double t);

// The interpolated surface at (k, t). q = w' / w is q_num / q_den. Before the first slice the
// factor lambda cancels, so q_num, q_den are w_0' and w_0 themselves, and g stays defined at
// t = 0, where w = 0. q_den is the variance whose positivity the floor rule tests.
template <class Real>
struct SurfacePoint {
  Real w;
  Real w1;  // dw / dk
  Real w2;  // d2w / dk2
  Real dwdt;
  Real q_num;
  Real q_den;
};

// A slice with what every evaluation reuses: sigma^2 and b sigma^2, recorded once in the set-up.
template <class Real>
struct SliceSetup {
  RawSvi<Real> p;
  Real sigma2;
  Real b_sigma2;
};

template <class Real>
SliceSetup<Real> MakeSliceSetup(const RawSvi<Real>& p) {
  const Real sigma2 = p.sigma * p.sigma;
  return {.p = p, .sigma2 = sigma2, .b_sigma2 = p.b * sigma2};
}

// w, w' and w'' of one slice (raw_svi.hpp's formulas, on the set-up's sigma^2).
template <class Real>
SviPoint<Real> EvaluateSlice(const SliceSetup<Real>& s, const Real& k) {
  const Real x = k - s.p.m;
  const Real r = Sqrt(x * x + s.sigma2);
  return {.w = s.p.a + s.p.b * (s.p.rho * x + r),
          .w1 = s.p.b * (s.p.rho + x / r),
          .w2 = s.b_sigma2 / (r * r * r)};
}

template <class Real>
class SviSurface {
 public:
  // Precondition: ValidateSurface(terms, the double values of params) passed.
  SviSurface(std::span<const SliceTerms> terms, std::span<const RawSvi<Real>> params)
      : curve_(terms) {
    expiries_.reserve(terms.size());
    slices_.reserve(params.size());
    for (const SliceTerms& t : terms) expiries_.push_back(t.expiry);
    for (const RawSvi<Real>& p : params) slices_.push_back(MakeSliceSetup(p));
  }

  [[nodiscard]] std::size_t Slices() const { return slices_.size(); }
  [[nodiscard]] std::span<const double> Expiries() const { return expiries_; }
  [[nodiscard]] const ForwardCurve& Curve() const { return curve_; }
  [[nodiscard]] const SliceSetup<Real>& Slice(std::size_t j) const { return slices_[j]; }
  [[nodiscard]] TimeSlot Locate(double t) const { return svi::Locate(expiries_, t); }

  [[nodiscard]] SurfacePoint<Real> At(const TimeSlot& slot, const Real& k) const {
    if (slot.segment == 0) {
      const SviPoint<Real> s = EvaluateSlice(slices_[0], k);
      const double lambda = slot.weight;
      return {.w = lambda * s.w,
              .w1 = lambda * s.w1,
              .w2 = lambda * s.w2,
              .dwdt = s.w * slot.inverse_length,
              .q_num = s.w1,
              .q_den = s.w};
    }
    const SviPoint<Real> lo = EvaluateSlice(slices_[slot.segment - 1], k);
    const SviPoint<Real> hi = EvaluateSlice(slices_[slot.segment], k);
    const double alpha = slot.weight;
    const double beta = 1.0 - alpha;
    const Real w = beta * lo.w + alpha * hi.w;
    const Real w1 = beta * lo.w1 + alpha * hi.w1;
    return {.w = w,
            .w1 = w1,
            .w2 = beta * lo.w2 + alpha * hi.w2,
            .dwdt = (hi.w - lo.w) * slot.inverse_length,
            .q_num = w1,
            .q_den = w};
  }

 private:
  ForwardCurve curve_;
  std::vector<double> expiries_;
  std::vector<SliceSetup<Real>> slices_;
};

}  // namespace aadp::svi

#endif  // AADP_SVI_SURFACE_HPP_
