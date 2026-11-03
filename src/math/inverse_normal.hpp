// Inverse standard normal for double: Wichura, Algorithm AS 241 (PPND16), Applied Statistics
// 37(3), 1988, with the tails on this directory's log and IEEE sqrt.
//
// The same rationals, coefficients and operation order as the frozen reference in
// src/rng/normal.hpp, so the central region gives the same bits as the reference, and the tails
// do whenever math::log and std::log agree. tests/math/inverse_normal_test.cpp checks the
// coefficients are identical at compile time.
//
// Select form. All three regions are degree 7 over degree 7, so every lane evaluates one rational,
// with its argument and each coefficient picked by region, and every lane takes the log and sqrt.
// A SIMD lane then runs the same operations in the same order as this scalar code and gets the
// same bits. A vector version may skip the log and sqrt when no lane is in a tail: that changes no
// lane's value.
//
// Accuracy target, stated before measuring: 1.0e-15 maximum relative error on IEEE double, for
// every p in (0, 1). Wichura's 6.0e-16 (BENCHMARKS.md RNG-3) is a sample maximum over 50,000
// points on base-16 arithmetic, which truncates where IEEE rounds. Here the sample is wider and
// includes the branch joins, and the tails add the rounding of log and sqrt: an ULP of r near
// p = 0.075, where x changes by about as much as r, is about 2.2e-16 of x. 6.0e-16 plus that is
// 8.2e-16; 1.0e-15 adds a margin. RNG-3 itself is not changed and is reported beside the result.
//
// Edge contract: NaN, p < 0 and p > 1 give the canonical quiet NaN. p = 0 gives -inf and p = 1
// gives +inf. p = 1/2 gives +0. Subnormal p is in the domain and gives a finite result.
#ifndef AADP_MATH_INVERSE_NORMAL_HPP_
#define AADP_MATH_INVERSE_NORMAL_HPP_

#include <array>
#include <cstddef>

#include "math/common.hpp"
#include "math/log.hpp"
#include "math/sqrt.hpp"

namespace aadp::math {

namespace inverse_normal_detail {

// Wichura's coefficients, digit for digit as in src/rng/normal.hpp.
inline constexpr std::array<double, 8> kCentralNum = {
    3.3871328727963666080e0,  1.3314166789178437745e+2, 1.9715909503065514427e+3,
    1.3731693765509461125e+4, 4.5921953931549871457e+4, 6.7265770927008700853e+4,
    3.3430575583588128105e+4, 2.5090809287301226727e+3};
inline constexpr std::array<double, 8> kCentralDen = {1.0,
                                                      4.2313330701600911252e+1,
                                                      6.8718700749205790830e+2,
                                                      5.3941960214247511077e+3,
                                                      2.1213794301586595867e+4,
                                                      3.9307895800092710610e+4,
                                                      2.8729085735721942674e+4,
                                                      5.2264952788528545610e+3};
inline constexpr std::array<double, 8> kMiddleNum = {
    1.42343711074968357734e0,  4.63033784615654529590e0, 5.76949722146069140550e0,
    3.64784832476320460504e0,  1.27045825245236838258e0, 2.41780725177450611770e-1,
    2.27238449892691845833e-2, 7.74545014278341407640e-4};
inline constexpr std::array<double, 8> kMiddleDen = {1.0,
                                                     2.05319162663775882187e0,
                                                     1.67638483018380384940e0,
                                                     6.89767334985100004550e-1,
                                                     1.48103976427480074590e-1,
                                                     1.51986665636164571966e-2,
                                                     5.47593808499534494600e-4,
                                                     1.05075007164441684324e-9};
inline constexpr std::array<double, 8> kTailNum = {
    6.65790464350110377720e0,  5.46378491116411436990e0,  1.78482653991729133580e0,
    2.96560571828504891230e-1, 2.65321895265761230930e-2, 1.24266094738807843860e-3,
    2.71155556874348757815e-5, 2.01033439929228813265e-7};
inline constexpr std::array<double, 8> kTailDen = {1.0,
                                                   5.99832206555887937690e-1,
                                                   1.36929880922735805310e-1,
                                                   1.48753612908506148525e-2,
                                                   7.86869131145613259100e-4,
                                                   1.84631831751005468180e-5,
                                                   1.42151175831644588870e-7,
                                                   2.04426310338993978564e-15};

inline constexpr double kCentralBound = 0.425;     // |p - 1/2| at the central branch's edge
inline constexpr double kCentralShift = 0.180625;  // 0.425^2
inline constexpr double kMiddleShift = 1.6;
inline constexpr double kFarBound = 5.0;  // r = sqrt(-log tail) at the far tail's edge

}  // namespace inverse_normal_detail

inline double inverse_normal(double p) {
  using namespace inverse_normal_detail;
  using detail::kInf;
  const double q = p - 0.5;
  const bool central = (q < 0.0 ? -q : q) <= kCentralBound;
  const bool lower = q < 0.0;
  const double tail = lower ? p : 1.0 - p;
  const bool in_tail = !central && tail > 0.0;

  const double r = math::sqrt(-math::log(in_tail ? tail : 0.5));
  const bool far = r > kFarBound;
  const double tail_t = far ? r - kFarBound : r - kMiddleShift;
  const double t = central ? kCentralShift - q * q : tail_t;

  // One rational, its coefficients picked per region, Horner from the top as in the reference.
  const auto pick = [&](const std::array<double, 8>& c, const std::array<double, 8>& m,
                        const std::array<double, 8>& f, std::size_t i) {
    const double tail_coefficient = far ? f[i] : m[i];
    return central ? c[i] : tail_coefficient;
  };
  double num = pick(kCentralNum, kMiddleNum, kTailNum, 7);
  double den = pick(kCentralDen, kMiddleDen, kTailDen, 7);
  for (std::size_t i = 7; i-- > 0;) {
    num = num * t + pick(kCentralNum, kMiddleNum, kTailNum, i);
    den = den * t + pick(kCentralDen, kMiddleDen, kTailDen, i);
  }
  // The reference computes q * N / D left to right, so the central numerator takes q first.
  const double ratio = (central ? q * num : num) / den;

  double x = (!central && lower) ? -ratio : ratio;
  const double infinity = lower ? -kInf : kInf;
  x = (!central && tail == 0.0) ? infinity : x;
  return (p >= 0.0 && p <= 1.0) ? x : detail::kNaN;
}

}  // namespace aadp::math

#endif  // AADP_MATH_INVERSE_NORMAL_HPP_
