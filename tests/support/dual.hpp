// Forward-mode dual number with five tangents, one per first-order Greek input.
//
// This is a stand-in for the Phase 3 tape so the oracles can be proved first. It is not AAD:
// it pushes tangents forward and records nothing. It lives under tests/ for that reason.
//
// Policy, checked by tests/oracle/dual_test.cpp against double:
// - The value of every operation is the double operation on the values, bit for bit.
// - A zero tangent stays zero, even where the local derivative is inf or NaN. A constant has
//   no derivative, so sqrt(Dual(0.0)) must not poison the result with 0 * inf = NaN.
// - A non-zero tangent multiplies the local derivative under IEEE rules, so NaN and inf
//   propagate as they would from the formula.
#ifndef AADP_TESTS_SUPPORT_DUAL_HPP_
#define AADP_TESTS_SUPPORT_DUAL_HPP_

#include <array>
#include <cmath>
#include <cstddef>

#include "rng/normal.hpp"

namespace aadp::test {

// Keeps the double overloads visible beside the Dual ones declared below.
using aadp::inverse_normal;
using aadp::normal_cdf;
using aadp::normal_pdf;

inline constexpr std::size_t kTangents = 5;

struct Dual {
  double value = 0.0;
  std::array<double, kTangents> tangent{};

  constexpr Dual() = default;
  // Implicit, because mixed arithmetic with double is the point.
  // NOLINTNEXTLINE(cppcoreguidelines-explicit-constructor,misc-explicit-constructor): see above.
  constexpr Dual(double v) : value(v) {}

  static constexpr Dual Variable(double v, std::size_t slot) {
    Dual x(v);
    x.tangent[slot] = 1.0;
    return x;
  }
};

namespace dual_detail {

constexpr double Times(double tangent, double local) {
  return tangent == 0.0 ? 0.0 : tangent * local;
}

constexpr Dual Chain(double value, const Dual& x, double local) {
  Dual out(value);
  for (std::size_t i = 0; i < kTangents; ++i) out.tangent[i] = Times(x.tangent[i], local);
  return out;
}

constexpr Dual Chain2(double value, const Dual& a, double local_a, const Dual& b, double local_b) {
  Dual out(value);
  for (std::size_t i = 0; i < kTangents; ++i) {
    out.tangent[i] = Times(a.tangent[i], local_a) + Times(b.tangent[i], local_b);
  }
  return out;
}

}  // namespace dual_detail

constexpr Dual operator+(const Dual& x) { return x; }
constexpr Dual operator-(const Dual& x) { return dual_detail::Chain(-x.value, x, -1.0); }

constexpr Dual operator+(const Dual& a, const Dual& b) {
  return dual_detail::Chain2(a.value + b.value, a, 1.0, b, 1.0);
}
constexpr Dual operator-(const Dual& a, const Dual& b) {
  return dual_detail::Chain2(a.value - b.value, a, 1.0, b, -1.0);
}
constexpr Dual operator*(const Dual& a, const Dual& b) {
  return dual_detail::Chain2(a.value * b.value, a, b.value, b, a.value);
}
constexpr Dual operator/(const Dual& a, const Dual& b) {
  return dual_detail::Chain2(a.value / b.value, a, 1.0 / b.value, b,
                             -a.value / (b.value * b.value));
}

constexpr Dual& operator+=(Dual& a, const Dual& b) { return a = a + b; }
constexpr Dual& operator-=(Dual& a, const Dual& b) { return a = a - b; }
constexpr Dual& operator*=(Dual& a, const Dual& b) { return a = a * b; }
constexpr Dual& operator/=(Dual& a, const Dual& b) { return a = a / b; }

constexpr bool operator==(const Dual& a, const Dual& b) { return a.value == b.value; }
constexpr bool operator!=(const Dual& a, const Dual& b) { return a.value != b.value; }
constexpr bool operator<(const Dual& a, const Dual& b) { return a.value < b.value; }
constexpr bool operator<=(const Dual& a, const Dual& b) { return a.value <= b.value; }
constexpr bool operator>(const Dual& a, const Dual& b) { return a.value > b.value; }
constexpr bool operator>=(const Dual& a, const Dual& b) { return a.value >= b.value; }

inline Dual exp(const Dual& x) {
  const double value = std::exp(x.value);
  return dual_detail::Chain(value, x, value);
}
inline Dual expm1(const Dual& x) {
  return dual_detail::Chain(std::expm1(x.value), x, std::exp(x.value));
}
inline Dual log(const Dual& x) { return dual_detail::Chain(std::log(x.value), x, 1.0 / x.value); }
inline Dual log1p(const Dual& x) {
  return dual_detail::Chain(std::log1p(x.value), x, 1.0 / (1.0 + x.value));
}
inline Dual sqrt(const Dual& x) {
  const double value = std::sqrt(x.value);
  return dual_detail::Chain(value, x, 0.5 / value);
}
inline Dual fabs(const Dual& x) {
  return dual_detail::Chain(std::fabs(x.value), x, x.value < 0.0 ? -1.0 : 1.0);
}
inline Dual abs(const Dual& x) { return fabs(x); }

// Same selection rule as std::min and std::max, including which argument a NaN falls through to.
constexpr Dual max(const Dual& a, const Dual& b) { return a < b ? b : a; }
constexpr Dual min(const Dual& a, const Dual& b) { return b < a ? b : a; }

inline Dual pow(const Dual& base, double exponent) {
  return dual_detail::Chain(std::pow(base.value, exponent), base,
                            exponent * std::pow(base.value, exponent - 1.0));
}
inline Dual pow(double base, const Dual& exponent) {
  const double value = std::pow(base, exponent.value);
  return dual_detail::Chain(value, exponent, value * std::log(base));
}
inline Dual pow(const Dual& base, const Dual& exponent) {
  const double value = std::pow(base.value, exponent.value);
  return dual_detail::Chain2(value, base,
                             exponent.value * std::pow(base.value, exponent.value - 1.0), exponent,
                             value * std::log(base.value));
}

inline Dual normal_pdf(const Dual& x) {
  const double value = aadp::normal_pdf(x.value);
  return dual_detail::Chain(value, x, -x.value * value);
}
inline Dual normal_cdf(const Dual& x) {
  return dual_detail::Chain(aadp::normal_cdf(x.value), x, aadp::normal_pdf(x.value));
}
inline Dual inverse_normal(const Dual& p) {
  const double value = aadp::inverse_normal(p.value);
  return dual_detail::Chain(value, p, 1.0 / aadp::normal_pdf(value));
}

}  // namespace aadp::test

#endif  // AADP_TESTS_SUPPORT_DUAL_HPP_
