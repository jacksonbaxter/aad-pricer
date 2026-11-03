// The committed reference table, and how an error is measured against it.
//
// tools/math_reference/generate.py writes reference_values.inc in decimal arithmetic at 70 digits,
// and the ctest aadp_math_reference_current regenerates it, so it cannot drift. Each exact value
// is stored as hi + lo, hi the nearest double, which resolves errors to about 1e-16 ULP.
#ifndef AADP_TESTS_MATH_REFERENCE_TABLE_HPP_
#define AADP_TESTS_MATH_REFERENCE_TABLE_HPP_

#include <array>
#include <cmath>
#include <limits>

namespace aadp::math::testing {

// A constructor rather than an aggregate, so the generated rows stay plain {x, hi, lo, domain}.
struct ReferencePoint {
  constexpr ReferencePoint(double input_value, double hi_value, double lo_value, int domain_index)
      : input(input_value), hi(hi_value), lo(lo_value), domain(domain_index) {}
  double input;
  double hi;
  double lo;
  int domain;
};

inline constexpr double kInf = std::numeric_limits<double>::infinity();

#include "reference_values.inc"  // IWYU pragma: keep

// Spacing of doubles at the exact value hi + lo. When hi is a power of two and the exact value
// lies just below it, the spacing is that of the binade below.
inline double UlpOfExact(double hi, double lo) {
  const double magnitude = std::fabs(hi);
  if (magnitude < 0x1p-1022) return 0x1p-1074;
  int exponent = 0;
  const double fraction = std::frexp(magnitude, &exponent);
  double ulp = std::ldexp(1.0, exponent - 53);
  if (fraction == 0.5 && lo != 0.0 && std::signbit(lo) != std::signbit(hi)) ulp *= 0.5;
  return ulp < 0x1p-1074 ? 0x1p-1074 : ulp;
}

// |y - (hi + lo)| in ULPs of the exact value. y - hi is exact while y is within a factor of two of
// hi, which every error that could pass a test is. Infinite or zero hi must be met exactly, and a
// NaN y is an infinite error, so it cannot slip past a maximum.
inline double UlpError(double y, double hi, double lo) {
  if (std::isnan(y)) return std::numeric_limits<double>::infinity();
  if (std::isinf(hi) || hi == 0.0) {
    return y == hi ? 0.0 : std::numeric_limits<double>::infinity();
  }
  return std::fabs((y - hi) - lo) / UlpOfExact(hi, lo);
}

inline double RelativeError(double y, double hi, double lo) {
  if (std::isnan(y)) return std::numeric_limits<double>::infinity();
  if (std::isinf(hi) || hi == 0.0) {
    return y == hi ? 0.0 : std::numeric_limits<double>::infinity();
  }
  return std::fabs((y - hi) - lo) / std::fabs(hi);
}

}  // namespace aadp::math::testing

#endif  // AADP_TESTS_MATH_REFERENCE_TABLE_HPP_
