// Forward-mode dual number with N tangent lanes over a scalar T, namespace aadp::ad.
//
// It is the value type of the second-order tape (second_order.hpp): recording the pricer over
// Dual<double, N> values gives every value N directional derivatives, and the tape's partials
// become Duals, so one reverse sweep then gives the gradient and N Hessian-vector products
// (forward over reverse). Nested, Dual<Dual<double, 1>, 1> carries second derivatives in forward
// mode, which the tests use to differentiate closed forms twice.
//
// Values are the pricer's double operations on the values, bit for bit: the same src/math/ exp,
// log, sqrt and inverse normal and the pricer's normal CDF, and phi(x) = exp(-x^2 / 2) /
// sqrt(2 pi) written as src/ad/real.hpp writes it. So a Dual kernel's value, and the value part
// of every partial the second-order tape records, is the first-order tape's double.
//
// Tangents follow IEEE rules: a zero tangent times an infinite local derivative is NaN, as an
// adjoint of zero times an infinite partial is on the first-order tape. Lanes never mix.
//
// kAbsolute = true is the same type for error bounds: every tangent rule is applied to absolute
// values, so a tangent is the sum of |terms| of the derivative it stands for (each partial and
// each seed taken in absolute value), the second-order analogue of Tape::SweepForwardAbsolute.
// Values are unchanged. Only T = double has an absolute form.
#ifndef AADP_AD_DUAL_HPP_
#define AADP_AD_DUAL_HPP_

#include <array>
#include <cmath>
#include <compare>
#include <cstddef>
#include <numbers>
#include <type_traits>

#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "pricer/normal_cdf.hpp"

namespace aadp::ad {

template <class T, std::size_t N, bool kAbsolute = false>
struct Dual {
  static_assert(!kAbsolute || std::is_same_v<T, double>, "only a double Dual has an absolute form");
  static constexpr std::size_t kLanes = N;
  static constexpr bool kIsAbsolute = kAbsolute;

  T value{};
  std::array<T, N> tangent{};

  constexpr Dual() = default;
  // Implicit, so that kernels mix Dual and double freely; a double is a constant.
  // NOLINTNEXTLINE(cppcoreguidelines-explicit-constructor,misc-explicit-constructor): see above.
  constexpr Dual(double v) : value(v) {}
};

namespace dual_detail {

inline constexpr double kInvSqrt2Pi = std::numbers::inv_sqrtpi / std::numbers::sqrt2;

template <class X>
struct IsDual : std::false_type {};
template <class T, std::size_t N, bool A>
struct IsDual<Dual<T, N, A>> : std::true_type {};

// The value operations, on double or, by argument-dependent lookup, on a nested Dual.
inline double ExpOf(double x) { return math::exp(x); }
inline double LogOf(double x) { return math::log(x); }
inline double SqrtOf(double x) { return math::sqrt(x); }
inline double InverseNormalOf(double p) { return math::inverse_normal(p); }
inline double NormalCdfOf(double x) { return pricer::NormalCdf(x); }
inline double PdfOf(double x) { return math::exp(-0.5 * x * x) * kInvSqrt2Pi; }
inline double Magnitude(double x) { return std::fabs(x); }
inline double Innermost(double x) { return x; }

template <class T, std::size_t N, bool A>
Dual<T, N, A> ExpOf(const Dual<T, N, A>& x);
template <class T, std::size_t N, bool A>
Dual<T, N, A> LogOf(const Dual<T, N, A>& x);
template <class T, std::size_t N, bool A>
Dual<T, N, A> SqrtOf(const Dual<T, N, A>& x);
template <class T, std::size_t N, bool A>
Dual<T, N, A> InverseNormalOf(const Dual<T, N, A>& p);
template <class T, std::size_t N, bool A>
Dual<T, N, A> NormalCdfOf(const Dual<T, N, A>& x);
template <class T, std::size_t N, bool A>
Dual<T, N, A> PdfOf(const Dual<T, N, A>& x);
template <class T, std::size_t N, bool A>
double Innermost(const Dual<T, N, A>& x) {
  return Innermost(x.value);
}

// out.tangent = x.tangent * local, lane by lane.
template <class T, std::size_t N, bool A>
Dual<T, N, A> Chain(const T& value, const Dual<T, N, A>& x, const T& local) {
  Dual<T, N, A> out;
  out.value = value;
  for (std::size_t k = 0; k < N; ++k) {
    if constexpr (A) {
      out.tangent[k] = x.tangent[k] * Magnitude(local);
    } else {
      out.tangent[k] = x.tangent[k] * local;
    }
  }
  return out;
}

}  // namespace dual_detail

// Arithmetic. Each lane is the derivative rule on that lane alone.
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator+(const Dual<T, N, A>& a) {
  return a;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator-(const Dual<T, N, A>& a) {
  Dual<T, N, A> out;
  out.value = -a.value;
  for (std::size_t k = 0; k < N; ++k) out.tangent[k] = A ? a.tangent[k] : -a.tangent[k];
  return out;
}

template <class T, std::size_t N, bool A>
Dual<T, N, A> operator+(const Dual<T, N, A>& a, const Dual<T, N, A>& b) {
  Dual<T, N, A> out;
  out.value = a.value + b.value;
  for (std::size_t k = 0; k < N; ++k) out.tangent[k] = a.tangent[k] + b.tangent[k];
  return out;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator-(const Dual<T, N, A>& a, const Dual<T, N, A>& b) {
  Dual<T, N, A> out;
  out.value = a.value - b.value;
  for (std::size_t k = 0; k < N; ++k) {
    out.tangent[k] = A ? a.tangent[k] + b.tangent[k] : a.tangent[k] - b.tangent[k];
  }
  return out;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator*(const Dual<T, N, A>& a, const Dual<T, N, A>& b) {
  using dual_detail::Magnitude;
  Dual<T, N, A> out;
  out.value = a.value * b.value;
  for (std::size_t k = 0; k < N; ++k) {
    if constexpr (A) {
      out.tangent[k] = (a.tangent[k] * Magnitude(b.value)) + (Magnitude(a.value) * b.tangent[k]);
    } else {
      out.tangent[k] = (a.tangent[k] * b.value) + (a.value * b.tangent[k]);
    }
  }
  return out;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator/(const Dual<T, N, A>& a, const Dual<T, N, A>& b) {
  using dual_detail::Magnitude;
  Dual<T, N, A> out;
  out.value = a.value / b.value;
  for (std::size_t k = 0; k < N; ++k) {
    if constexpr (A) {
      out.tangent[k] =
          (a.tangent[k] + (Magnitude(out.value) * b.tangent[k])) / Magnitude(b.value);
    } else {
      out.tangent[k] = (a.tangent[k] - (out.value * b.tangent[k])) / b.value;
    }
  }
  return out;
}

// With a double constant on either side.
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator+(const Dual<T, N, A>& a, double b) {
  Dual<T, N, A> out = a;
  out.value = a.value + b;
  return out;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator+(double a, const Dual<T, N, A>& b) {
  Dual<T, N, A> out = b;
  out.value = a + b.value;
  return out;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator-(const Dual<T, N, A>& a, double b) {
  Dual<T, N, A> out = a;
  out.value = a.value - b;
  return out;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator-(double a, const Dual<T, N, A>& b) {
  Dual<T, N, A> out = -b;
  out.value = a - b.value;
  return out;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator*(const Dual<T, N, A>& a, double b) {
  Dual<T, N, A> out;
  out.value = a.value * b;
  for (std::size_t k = 0; k < N; ++k) out.tangent[k] = a.tangent[k] * (A ? std::fabs(b) : b);
  return out;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator*(double a, const Dual<T, N, A>& b) {
  Dual<T, N, A> out;
  out.value = a * b.value;
  for (std::size_t k = 0; k < N; ++k) out.tangent[k] = (A ? std::fabs(a) : a) * b.tangent[k];
  return out;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator/(const Dual<T, N, A>& a, double b) {
  Dual<T, N, A> out;
  out.value = a.value / b;
  for (std::size_t k = 0; k < N; ++k) out.tangent[k] = a.tangent[k] / (A ? std::fabs(b) : b);
  return out;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> operator/(double a, const Dual<T, N, A>& b) {
  return Dual<T, N, A>(a) / b;
}

template <class T, std::size_t N, bool A>
Dual<T, N, A>& operator+=(Dual<T, N, A>& a, const Dual<T, N, A>& b) {
  return a = a + b;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A>& operator-=(Dual<T, N, A>& a, const Dual<T, N, A>& b) {
  return a = a - b;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A>& operator*=(Dual<T, N, A>& a, const Dual<T, N, A>& b) {
  return a = a * b;
}
template <class T, std::size_t N, bool A>
Dual<T, N, A>& operator/=(Dual<T, N, A>& a, const Dual<T, N, A>& b) {
  return a = a / b;
}

// Comparisons read the innermost value only, with IEEE semantics.
template <class T, std::size_t N, bool A>
bool operator==(const Dual<T, N, A>& a, const Dual<T, N, A>& b) {
  return dual_detail::Innermost(a) == dual_detail::Innermost(b);
}
template <class T, std::size_t N, bool A>
std::partial_ordering operator<=>(const Dual<T, N, A>& a, const Dual<T, N, A>& b) {
  return dual_detail::Innermost(a) <=> dual_detail::Innermost(b);
}
template <class T, std::size_t N, bool A>
bool operator==(const Dual<T, N, A>& a, double b) {
  return dual_detail::Innermost(a) == b;
}
template <class T, std::size_t N, bool A>
std::partial_ordering operator<=>(const Dual<T, N, A>& a, double b) {
  return dual_detail::Innermost(a) <=> b;
}

// Elementary functions: value by the pricer's own kernel, tangent by the exact derivative at it.
template <class T, std::size_t N, bool A>
Dual<T, N, A> Exp(const Dual<T, N, A>& x) {
  const T value = dual_detail::ExpOf(x.value);
  return dual_detail::Chain(value, x, value);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> Log(const Dual<T, N, A>& x) {
  return dual_detail::Chain(dual_detail::LogOf(x.value), x, T(1.0) / x.value);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> Sqrt(const Dual<T, N, A>& x) {
  const T value = dual_detail::SqrtOf(x.value);
  return dual_detail::Chain(value, x, T(0.5) / value);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> NormalCdf(const Dual<T, N, A>& x) {
  return dual_detail::Chain(dual_detail::NormalCdfOf(x.value), x, dual_detail::PdfOf(x.value));
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> InverseNormal(const Dual<T, N, A>& p) {
  const T value = dual_detail::InverseNormalOf(p.value);
  return dual_detail::Chain(value, p, T(1.0) / dual_detail::PdfOf(value));
}
// phi(x), written as src/ad/real.hpp's NormalPdf writes it, so its value is that double.
template <class T, std::size_t N, bool A>
Dual<T, N, A> NormalPdf(const Dual<T, N, A>& x) {
  return Exp(-0.5 * x * x) * dual_detail::kInvSqrt2Pi;
}

template <class T, std::size_t N, bool A>
double Value(const Dual<T, N, A>& x) {
  return dual_detail::Innermost(x);
}

namespace dual_detail {

template <class T, std::size_t N, bool A>
Dual<T, N, A> ExpOf(const Dual<T, N, A>& x) {
  return Exp(x);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> LogOf(const Dual<T, N, A>& x) {
  return Log(x);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> SqrtOf(const Dual<T, N, A>& x) {
  return Sqrt(x);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> InverseNormalOf(const Dual<T, N, A>& p) {
  return InverseNormal(p);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> NormalCdfOf(const Dual<T, N, A>& x) {
  return NormalCdf(x);
}
template <class T, std::size_t N, bool A>
Dual<T, N, A> PdfOf(const Dual<T, N, A>& x) {
  return NormalPdf(x);
}

}  // namespace dual_detail

}  // namespace aadp::ad

#endif  // AADP_AD_DUAL_HPP_
