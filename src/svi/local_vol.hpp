// Dupire local variance on the interpolated raw SVI surface, with the floor rule, as docs/NOTES.md,
// "Stage 7a", pre-registers it. Templated on Real, so the tape records it.
//
//   sigma_loc^2(t, k) = dT w / g,  g = (1 - k q / 2)^2 - w' q / 4 - w'^2 / 16 + w'' / 2,  q = w'/w
//
// g is Gatheral and Jacquier's eq. (2.1), arXiv 1204.0646, with 1/w written through q, the
// working denominator of QuantLib's LocalVolSurface and of the R code in Gatheral's VW4 notes.
// VW4's displayed eq. (12) has the opposite sign on the (1/4)(1/4 + 1/w) w'^2 term; the minus
// sign here is the one its code, QuantLib and g share (docs/NOTES.md, "SVI"). k is forward
// log-moneyness, so rates and carry enter only through k.
//
// Floor rule, on the double values, which are the same bits for every Real, in this order:
//   non-finite w_pos, dT w or g          floored
//   w_pos <= omega                       floored, non-positive variance
//   dT w <= omega                        floored, calendar
//   g <= omega                           floored, butterfly
//   dT w <= floor g                      floored, below the floor
//   dT w >= cap g                        capped
//   otherwise                            dT w / g
// with omega = 2^-200, w_pos the q_den of surface.hpp. Floored and capped points take the floor
// or the cap as a constant, so they carry no derivative. omega keeps every recorded partial
// finite: the tape's reverse sweep multiplies each partial by its adjoint even where that is zero,
// so an infinite partial on a branch Select did not take would still make a Greek NaN.
#ifndef AADP_SVI_LOCAL_VOL_HPP_
#define AADP_SVI_LOCAL_VOL_HPP_

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "svi/surface.hpp"

namespace aadp::svi {

struct LocalVolRule {
  double floor = 1e-4;  // per year: 1% local volatility
  double cap = 4.0;     // per year: 200% local volatility
};

inline constexpr double kAdmissibleThreshold = 0x1p-200;

enum class LocalVarianceClass : std::uint8_t {
  kOk,
  kNonFinite,
  kNonPositiveVariance,
  kCalendar,
  kButterfly,
  kBelowFloor,
  kCapped,
};
inline constexpr std::size_t kLocalVarianceClasses = 7;

[[nodiscard]] const char* LocalVarianceClassName(LocalVarianceClass c) noexcept;

constexpr bool IsFloored(LocalVarianceClass c) {
  return c != LocalVarianceClass::kOk && c != LocalVarianceClass::kCapped;
}

// The rule above on values. Steps 5 and 6 multiply rather than divide, so the ratio is formed
// only where it lies between the floor and the cap.
[[nodiscard]] LocalVarianceClass Classify(double w_pos, double dwdt, double g,
                                          const LocalVolRule& rule);

// Mask is bool for double, the tape and the Dual; their own overloads win where they exist.
template <class Real>
Real Select(bool mask, const Real& if_true, const Real& if_false) {
  return mask ? if_true : if_false;
}

// GJ eq. (2.1) through q = w' / w.
template <class Real>
Real Durrleman(const Real& q, const Real& w1, const Real& w2, const Real& k) {
  const Real u = 1.0 - 0.5 * (k * q);
  // ((u^2 - w1 q / 4) - w1^2 / 16) + w2 / 2, each operand that records named in clang's order, so
  // the tape's statement order does not depend on the compiler (src/pricer/kernels.hpp, Finish).
  const Real u2 = u * u;
  const Real slope = 0.25 * (w1 * q);
  const Real first = u2 - slope;
  const Real slope2 = 0.0625 * (w1 * w1);
  const Real second = first - slope2;
  const Real curvature = 0.5 * w2;
  return second + curvature;
}

template <class Real>
struct LocalVariance {
  Real variance;  // after the floor rule
  Real dwdt;
  Real g;
  LocalVarianceClass cls = LocalVarianceClass::kOk;
};

template <class Real>
LocalVariance<Real> LocalVarianceAt(const SurfacePoint<Real>& p, const Real& k,
                                    const LocalVolRule& rule) {
  const double w_pos = Value(p.q_den);
  const bool positive = std::isfinite(w_pos) && w_pos > kAdmissibleThreshold;
  const Real q = p.q_num / Select(positive, p.q_den, Real(1.0));
  const Real g = Durrleman(q, p.w1, p.w2, k);
  const LocalVarianceClass cls = Classify(w_pos, Value(p.dwdt), Value(g), rule);
  const bool ok = cls == LocalVarianceClass::kOk;
  const Real ratio = Select(ok, p.dwdt, Real(1.0)) / Select(ok, g, Real(1.0));
  const Real bound(cls == LocalVarianceClass::kCapped ? rule.cap : rule.floor);
  return {.variance = Select(ok, ratio, bound), .dwdt = p.dwdt, .g = g, .cls = cls};
}

}  // namespace aadp::svi

#endif  // AADP_SVI_LOCAL_VOL_HPP_
