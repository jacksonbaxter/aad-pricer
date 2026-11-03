// A Real for the pricer kernels that runs each operation twice and bounds the difference.
//
// own    the operation as the pricer does it: src/math/ exp, log, inverse normal, our normal CDF
// ref    the same operation on libm: std::exp, std::log, and src/rng/normal.hpp's inverse normal
//        and normal CDF, which are what the frozen reference kernels in src/mc/ call
// bound  an a-priori bound on |own - ref|, carried forward by first-order error analysis from
//        the accuracy of each function, stated below, and one rounding of each side per
//        operation. It never looks at ref, so it is a prediction, and ref only checks it.
//
// So a kernel instantiated on Tracked gives three things per path: own, which must be the double
// kernel's bits; ref, which must be the reference kernel's bits wherever the operations and their
// order are the same; and a bound that |own - ref| must respect.
//
// Comparisons give a TrackedMask, not a bool, so a kernel that branches on a Real does not compile
// here. A mask is unsure when the bound cannot rule out the other side taking the other branch;
// Select then widens the bound to cover either choice.
#ifndef AADP_TESTS_PRICER_ERROR_TRACKER_HPP_
#define AADP_TESTS_PRICER_ERROR_TRACKER_HPP_

#include <cmath>
#include <limits>
#include <numbers>

#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "pricer/normal_cdf.hpp"
#include "rng/normal.hpp"

namespace aadp::pricer::testing {

// The function accuracies the bound rests on, in ULP of the result unless stated. Each is a
// measured maximum, or a premise that a test measures; none is fitted to this test's outcome.
struct Accuracy {
  // Stage 1a, tests/math/exp_log_test.cpp, against exact values: 0.613 and 0.840 ULP.
  static constexpr double kOwnExp = 0.613;
  static constexpr double kOwnLog = 0.840;
  // Premise: libm's exp and log below 1 ULP. Checked on stage 1a's table in this test's suite.
  static constexpr double kLibmExp = 1.0;
  static constexpr double kLibmLog = 1.0;
  // AS241 relative error, own (stage 1a target 1.0e-15, measured 8.39e-16) and the reference on
  // libm (measured 8.47e-16 in Phase 2): 1.0e-15 for each.
  static constexpr double kInverseNormalRelative = 1.0e-15;
  // erfc in ULP: own, target 4 (normal_cdf_test.cpp), and libm, premise 4 (measured there).
  static constexpr double kOwnErfc = 4.0;
  static constexpr double kLibmErfc = 4.0;
};

inline constexpr double kUnitRoundoff = 0x1p-53;
inline constexpr double kSubnormal = 0x1p-1074;
inline constexpr double kInf = std::numeric_limits<double>::infinity();

struct Tracked {
  double own = 0.0;
  double ref = 0.0;
  double bound = 0.0;
  bool unsure = false;  // Some branch on the way may have gone the other way for ref.

  constexpr Tracked() = default;
  // NOLINTNEXTLINE(cppcoreguidelines-explicit-constructor,misc-explicit-constructor): constants.
  constexpr Tracked(double v) : own(v), ref(v) {}
  constexpr Tracked(double own_value, double ref_value, double bound_value, bool unsure_value)
      : own(own_value), ref(ref_value), bound(bound_value), unsure(unsure_value) {}
};

struct TrackedMask {
  bool own;
  bool ref;
  bool unsure;
};

namespace tracker_detail {

// Both sides round to nearest: each is within u of its exact result, and |own| bounds both to
// first order. A subnormal unit covers gradual underflow.
inline double Rounding(double own) { return (2.0 * kUnitRoundoff * std::fabs(own)) + kSubnormal; }

// Inputs with no difference give the same bits on both sides through the same IEEE operation.
inline Tracked Combine(double own, double ref, double propagated, const Tracked& a,
                       const Tracked& b) {
  const bool exact = a.bound == 0.0 && b.bound == 0.0;
  const double bound = exact ? 0.0 : (propagated * (1.0 + 4.0 * kUnitRoundoff)) + Rounding(own);
  return {own, ref, bound, a.unsure || b.unsure};
}

inline double Pdf(double x) { return std::exp(-0.5 * x * x) / std::sqrt(2.0 * std::numbers::pi); }

}  // namespace tracker_detail

inline Tracked operator+(const Tracked& a, const Tracked& b) {
  return tracker_detail::Combine(a.own + b.own, a.ref + b.ref, a.bound + b.bound, a, b);
}
inline Tracked operator-(const Tracked& a, const Tracked& b) {
  return tracker_detail::Combine(a.own - b.own, a.ref - b.ref, a.bound + b.bound, a, b);
}
inline Tracked operator-(const Tracked& a) { return {-a.own, -a.ref, a.bound, a.unsure}; }
inline Tracked operator*(const Tracked& a, const Tracked& b) {
  const double propagated =
      (std::fabs(a.own) * b.bound) + (std::fabs(b.own) * a.bound) + (a.bound * b.bound);
  return tracker_detail::Combine(a.own * b.own, a.ref * b.ref, propagated, a, b);
}
inline Tracked operator/(const Tracked& a, const Tracked& b) {
  const double own = a.own / b.own;
  const double margin = std::fabs(b.own) - b.bound;
  const double propagated = margin > 0.0 ? (a.bound + (std::fabs(own) * b.bound)) / margin : kInf;
  return tracker_detail::Combine(own, a.ref / b.ref, propagated, a, b);
}

inline TrackedMask Compare(bool own, bool ref, const Tracked& a, const Tracked& b) {
  const double slack = a.bound + b.bound;
  return {own, ref, slack > 0.0 && std::fabs(a.own - b.own) <= slack};
}
inline TrackedMask operator<(const Tracked& a, const Tracked& b) {
  return Compare(a.own < b.own, a.ref < b.ref, a, b);
}
inline TrackedMask operator>(const Tracked& a, const Tracked& b) {
  return Compare(a.own > b.own, a.ref > b.ref, a, b);
}
inline TrackedMask operator<=(const Tracked& a, const Tracked& b) {
  return Compare(a.own <= b.own, a.ref <= b.ref, a, b);
}
inline TrackedMask operator>=(const Tracked& a, const Tracked& b) {
  return Compare(a.own >= b.own, a.ref >= b.ref, a, b);
}

// Each side takes its own branch. If they might differ, the bound covers either choice.
inline Tracked Select(const TrackedMask& mask, const Tracked& if_true, const Tracked& if_false) {
  const Tracked& own_pick = mask.own ? if_true : if_false;
  const Tracked& ref_pick = mask.ref ? if_true : if_false;
  double bound = own_pick.bound;
  if (mask.unsure) {
    bound = std::fmax(if_true.bound, if_false.bound) + std::fabs(if_true.own - if_false.own);
  }
  return {own_pick.own, ref_pick.ref, bound, mask.unsure || if_true.unsure || if_false.unsure};
}

inline double Value(const Tracked& x) { return x.own; }

// |e^a - e^a'| <= e^a (e^d - 1), plus each side's own error at its argument.
inline Tracked Exp(const Tracked& a) {
  const double own = math::exp(a.own);
  const double grown = std::fabs(own) * (1.0 + 2.0 * kUnitRoundoff) * std::exp(a.bound);
  const double bound = (grown - std::fabs(own)) +
                       ((Accuracy::kOwnExp + Accuracy::kLibmExp) * 2.0 * kUnitRoundoff * grown) +
                       (2.0 * kSubnormal);
  return {own, std::exp(a.ref), bound, a.unsure};
}

inline Tracked Log(const Tracked& a) {
  const double own = math::log(a.own);
  const double margin = std::fabs(a.own) - a.bound;
  const double propagated = a.bound == 0.0 ? 0.0 : (margin > 0.0 ? a.bound / margin : kInf);
  const double bound = propagated +
                       ((Accuracy::kOwnLog + Accuracy::kLibmLog) * 2.0 * kUnitRoundoff *
                        (std::fabs(own) + propagated)) +
                       kSubnormal;
  return {own, std::log(a.ref), bound, a.unsure};
}

// IEEE square root on both sides: the same bits from the same input.
inline Tracked Sqrt(const Tracked& a) {
  const double own = math::sqrt(a.own);
  const double margin = a.own - a.bound;
  const double propagated =
      a.bound == 0.0 ? 0.0 : (margin > 0.0 ? a.bound / (2.0 * std::sqrt(margin)) : kInf);
  return tracker_detail::Combine(own, std::sqrt(a.ref), propagated, a, Tracked(0.0));
}

// The central region is the same code and coefficients on both sides, so the same input gives
// the same bits (stage 1a). Otherwise both are within kInverseNormalRelative of Phi^-1 at their
// own argument, and Phi^-1 moves by at most d / phi over an input difference d, with phi taken at
// the far end of the interval the first-order estimate gives.
inline Tracked InverseNormal(const Tracked& p) {
  const double own = math::inverse_normal(p.own);
  const double ref = aadp::inverse_normal(p.ref);
  const bool central = std::fabs(p.own - 0.5) <= 0.425;
  if (central && p.bound == 0.0) return {own, ref, 0.0, p.unsure};
  const double first = p.bound / tracker_detail::Pdf(own);
  const double far = std::fabs(own) + (2.0 * first);
  const double moved = p.bound == 0.0 ? 0.0 : p.bound / tracker_detail::Pdf(far);
  const double bound =
      moved + (2.0 * Accuracy::kInverseNormalRelative * (std::fabs(own) + moved)) + kSubnormal;
  return {own, ref, bound, p.unsure};
}

// Both sides compute y = -a / sqrt 2 the same way, so the same input gives the same y, and then
// differ by the two erfc errors. A difference d in a moves y by d / sqrt 2 plus the two
// divisions' roundings, and erfc(y) / 2 by sqrt 2 phi(a) per unit of y, with phi at the near end.
inline Tracked NormalCdf(const Tracked& a) {
  const double own = pricer::NormalCdf(a.own);
  const double ref = aadp::normal_cdf(a.ref);
  const double erfc_error =
      ((Accuracy::kOwnErfc + Accuracy::kLibmErfc) * 2.0 * kUnitRoundoff * own) + (4.0 * kSubnormal);
  if (a.bound == 0.0) return {own, ref, erfc_error, a.unsure};
  const double y = std::fabs(a.own) / std::numbers::sqrt2;
  const double y_moved =
      (a.bound / std::numbers::sqrt2 * (1.0 + 2.0 * kUnitRoundoff)) + (2.0 * kUnitRoundoff * y);
  const double near = std::fmax(std::fabs(a.own) - a.bound, 0.0);
  const double moved = std::numbers::sqrt2 * tracker_detail::Pdf(near) * y_moved;
  return {own, ref, erfc_error + moved, a.unsure};
}

}  // namespace aadp::pricer::testing

#endif  // AADP_TESTS_PRICER_ERROR_TRACKER_HPP_
