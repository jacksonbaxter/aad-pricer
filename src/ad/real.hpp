// The operations of the active type, everything src/pricer/real_ops.hpp asks of a Real.
//
// Values are the double operations of the pricer on the values, bit for bit: the same src/math/
// exp, log, sqrt and inverse normal, and the pricer's normal CDF. Each operation on an active
// operand records one statement whose partials are the analytic derivatives evaluated at those
// values, written exactly as the forward-mode Dual of tests/pricer/dual_ops.hpp writes them, so
// tape and Dual differ only in how the chain rule's products are associated:
//   a + b: 1, 1    a - b: 1, -1    a * b: b, a    a / b: 1 / b, -a / (b b)    -a: -1
//   exp x: exp x   log x: 1 / x    sqrt x: 0.5 / sqrt x
//   NormalCdf x: phi(x)            InverseNormal p: 1 / phi(InverseNormal p)
// with phi(x) = exp(-x^2 / 2) / sqrt(2 pi) on src/math/exp. A partial is only computed when
// an operand is active, so a passive call (a constant, a uniform) costs what the double does.
//
// Select returns the chosen operand itself, so its adjoint goes only there and nothing is
// recorded. Min and Max are Select on a comparison, with std::min and std::max's NaN rule.
#ifndef AADP_AD_REAL_HPP_
#define AADP_AD_REAL_HPP_

#include <compare>
#include <numbers>

#include "ad/tape.hpp"
#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "pricer/normal_cdf.hpp"

namespace aadp::ad {

namespace detail {

// Recording is inlined into every operation, and the four operations on two Reals are inlined
// into their callers: left to the inliner, each stayed a call, which spilled its floating-point
// arguments and cost more than the recording itself (docs/NOTES.md, "Stage 11"). Tape::Push
// enters a new chunk out of line, which keeps the inlined code small.
[[gnu::always_inline]] inline Real Record(double value, const Real& a, double da) {
  if (!a.active()) return {value};
  return ActiveTape().Push(value, a.index(), da);
}

[[gnu::always_inline]] inline Real Record(double value, const Real& a, double da, const Real& b,
                                          double db) {
  if (a.active() && b.active()) return ActiveTape().Push(value, a.index(), da, b.index(), db);
  if (!a.active() && !b.active()) return {value};
  // One operand is active, and only it is recorded: one push serves either, to keep the code small.
  const bool first = a.active();
  return ActiveTape().Push(value, first ? a.index() : b.index(), first ? da : db);
}

}  // namespace detail

inline double NormalPdf(double x) {
  return math::exp(-0.5 * x * x) * (std::numbers::inv_sqrtpi / std::numbers::sqrt2);
}

inline Real operator+(const Real& a) { return a; }
inline Real operator-(const Real& a) { return detail::Record(-a.value(), a, -1.0); }

[[gnu::always_inline]] inline Real operator+(const Real& a, const Real& b) {
  return detail::Record(a.value() + b.value(), a, 1.0, b, 1.0);
}
inline Real operator+(const Real& a, double b) { return detail::Record(a.value() + b, a, 1.0); }
inline Real operator+(double a, const Real& b) { return detail::Record(a + b.value(), b, 1.0); }

[[gnu::always_inline]] inline Real operator-(const Real& a, const Real& b) {
  return detail::Record(a.value() - b.value(), a, 1.0, b, -1.0);
}
inline Real operator-(const Real& a, double b) { return detail::Record(a.value() - b, a, 1.0); }
inline Real operator-(double a, const Real& b) { return detail::Record(a - b.value(), b, -1.0); }

[[gnu::always_inline]] inline Real operator*(const Real& a, const Real& b) {
  return detail::Record(a.value() * b.value(), a, b.value(), b, a.value());
}
inline Real operator*(const Real& a, double b) { return detail::Record(a.value() * b, a, b); }
inline Real operator*(double a, const Real& b) { return detail::Record(a * b.value(), b, a); }

[[gnu::always_inline]] inline Real operator/(const Real& a, const Real& b) {
  const double value = a.value() / b.value();
  if (!a.active() && !b.active()) return {value};
  return detail::Record(value, a, 1.0 / b.value(), b, -a.value() / (b.value() * b.value()));
}
inline Real operator/(const Real& a, double b) {
  const double value = a.value() / b;
  if (!a.active()) return {value};
  return detail::Record(value, a, 1.0 / b);
}
inline Real operator/(double a, const Real& b) {
  const double value = a / b.value();
  if (!b.active()) return {value};
  return detail::Record(value, b, -a / (b.value() * b.value()));
}

inline Real& operator+=(Real& a, const Real& b) { return a = a + b; }
inline Real& operator-=(Real& a, const Real& b) { return a = a - b; }
inline Real& operator*=(Real& a, const Real& b) { return a = a * b; }
inline Real& operator/=(Real& a, const Real& b) { return a = a / b; }

// Comparisons read values only. A double converts to a passive Real, so mixed comparisons work in
// either order, with IEEE semantics: anything against NaN is unordered.
inline bool operator==(const Real& a, const Real& b) { return a.value() == b.value(); }
inline std::partial_ordering operator<=>(const Real& a, const Real& b) {
  return a.value() <=> b.value();
}

inline Real Exp(const Real& x) {
  const double value = math::exp(x.value());
  return detail::Record(value, x, value);
}

inline Real Log(const Real& x) {
  const double value = math::log(x.value());
  if (!x.active()) return {value};
  return detail::Record(value, x, 1.0 / x.value());
}

inline Real Sqrt(const Real& x) {
  const double value = math::sqrt(x.value());
  if (!x.active()) return {value};
  return detail::Record(value, x, 0.5 / value);
}

inline Real NormalCdf(const Real& x) {
  const double value = pricer::NormalCdf(x.value());
  if (!x.active()) return {value};
  return detail::Record(value, x, NormalPdf(x.value()));
}

inline Real InverseNormal(const Real& p) {
  const double value = math::inverse_normal(p.value());
  if (!p.active()) return {value};
  return detail::Record(value, p, 1.0 / NormalPdf(value));
}

inline Real Select(bool mask, const Real& if_true, const Real& if_false) {
  return mask ? if_true : if_false;
}
inline Real Max(const Real& a, const Real& b) { return Select(a < b, b, a); }
inline Real Min(const Real& a, const Real& b) { return Select(b < a, b, a); }

inline double Value(const Real& x) { return x.value(); }

}  // namespace aadp::ad

#endif  // AADP_AD_REAL_HPP_
