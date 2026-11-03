// Standard normal CDF for double on the pricer's own exp, so no libm result reaches a price.
//
// Phi(x) = erfc(y) / 2 with y = -x / sqrt 2, rounded once, exactly as src/rng/normal.hpp does it,
// so the two compute erfc of the same double. erfc: W. J. Cody, "Rational Chebyshev
// approximations for the error function", Math. Comp. 23 (1969) 631-637, in the three regions
// and the coefficients of his CALERF routine (Netlib specfun): |y| <= 0.46875, up to 4, beyond
// 4. exp(-y^2) is taken as exp(-a^2) exp(-(y - a)(y + a)) with a = trunc(16 y) / 16, as Cody does,
// so the large square is never rounded. tests/pricer/normal_cdf_values.inc holds exact values
// from tests/pricer/normal_cdf_reference.py, so a mistyped coefficient shows as an error spike.
//
// Accuracy target, stated before measuring the committed table: erfc(y) within 4 ULP wherever
// Phi is normal, from two exps of 0.613 ULP each, the rational's few roundings and its fit,
// and the final product. Against Phi(x) itself the roundings of sqrt 2 and of y add a relative
// error of up to 4 y^2 2^-53, about 3e-13 at x = -37: the reference has the same, so path-by-path
// comparisons do not see it, but anything that needs Phi to full relative accuracy deep in the
// lower tail cannot use either. Measured in tests/pricer/normal_cdf_test.cpp.
//
// Edge contract: NaN gives the canonical quiet NaN. Phi(-inf) = +0 and Phi(+inf) = 1. Results
// below the normal range are gradual and lose relative accuracy, never flush.
//
// Select form, as in src/math/: every region is evaluated on a clamped argument and the result
// picked, so no branch depends on x and a SIMD lane can follow the same operations.
#ifndef AADP_PRICER_NORMAL_CDF_HPP_
#define AADP_PRICER_NORMAL_CDF_HPP_

#include <array>
#include <cmath>
#include <numbers>

#include "math/common.hpp"
#include "math/exp.hpp"

namespace aadp::pricer {

namespace normal_cdf_detail {

// Cody's coefficients, CALERF's A, B, C, D, P, Q, in its order.
inline constexpr std::array<double, 5> kSmallNum = {3.16112374387056560e00, 1.13864154151050156e02,
                                                    3.77485237685302021e02, 3.20937758913846947e03,
                                                    1.85777706184603153e-1};
inline constexpr std::array<double, 4> kSmallDen = {2.36012909523441209e01, 2.44024637934444173e02,
                                                    1.28261652607737228e03, 2.84423683343917062e03};
inline constexpr std::array<double, 9> kMiddleNum = {
    // NOLINTNEXTLINE(modernize-use-std-numbers): Cody's fitted coefficient, not 1/sqrt(pi).
    5.64188496988670089e-1, 8.88314979438837594e00, 6.61191906371416295e01,
    2.98635138197400131e02, 8.81952221241769090e02, 1.71204761263407058e03,
    2.05107837782607147e03, 1.23033935479799725e03, 2.15311535474403846e-8};
inline constexpr std::array<double, 8> kMiddleDen = {
    1.57449261107098347e01, 1.17693950891312499e02, 5.37181101862009858e02, 1.62138957456669019e03,
    3.29079923573345963e03, 4.36261909014324716e03, 3.43936767414372164e03, 1.23033935480374942e03};
inline constexpr std::array<double, 6> kFarNum = {3.05326634961232344e-1, 3.60344899949804439e-1,
                                                  1.25781726111229246e-1, 1.60837851487422766e-2,
                                                  6.58749161529837803e-4, 1.63153871373020978e-2};
inline constexpr std::array<double, 5> kFarDen = {2.56852019228982242e00, 1.87295284992346725e00,
                                                  5.27905102951428412e-1, 6.05183413124413191e-2,
                                                  2.33520497626869185e-3};
// NOLINTNEXTLINE(modernize-use-std-numbers): Cody's own rounding of 1/sqrt(pi), part of his fit.
inline constexpr double kInvSqrtPi = 5.6418958354775628695e-1;

inline constexpr double kSmallBound = 0.46875;
inline constexpr double kMiddleBound = 4.0;
// erfc(28) is about 6e-343, below half the smallest subnormal, so clamping there changes nothing.
inline constexpr double kZeroBound = 28.0;

// erfc for 0 <= y <= kZeroBound, |y| > kSmallBound, with y in the region the caller selects.
// Cody's loop order: num = (num + c[i]) * t for i = 0.., then the last coefficient added.
template <std::size_t N, std::size_t M>
double Rational(const std::array<double, N>& num_c, const std::array<double, M>& den_c, double t) {
  static_assert(N == M + 1);
  double num = num_c[N - 1] * t;
  double den = t;
  for (std::size_t i = 0; i + 2 < N; ++i) {
    num = (num + num_c[i]) * t;
    den = (den + den_c[i]) * t;
  }
  return (num + num_c[N - 2]) / (den + den_c[M - 1]);
}

}  // namespace normal_cdf_detail

inline double NormalCdf(double x) {
  using namespace normal_cdf_detail;
  const bool is_nan = x != x;
  const double y = -(is_nan ? 0.0 : x) / std::numbers::sqrt2;
  const double magnitude = y < 0.0 ? -y : y;
  const double ay = magnitude < kZeroBound ? magnitude : kZeroBound;

  // |y| <= 0.46875: erfc = 1 - erf, erf = y R(y^2).
  const double small_y = ay <= kSmallBound ? y : kSmallBound;
  const double small_t = small_y * small_y;
  const double small = 1.0 - small_y * Rational(kSmallNum, kSmallDen, small_t);

  // Beyond: erfc(|y|) = exp(-y^2) R(|y|), with R from the middle or far fit.
  const double middle_y = ay < kMiddleBound ? ay : kMiddleBound;
  const double middle = Rational(kMiddleNum, kMiddleDen, middle_y);
  const double far_y = ay > kMiddleBound ? ay : kMiddleBound;
  const double far_t = 1.0 / (far_y * far_y);
  const double far = (kInvSqrtPi - far_t * Rational(kFarNum, kFarDen, far_t)) / far_y;
  const double head = std::trunc(ay * 16.0) / 16.0;
  const double rest = (ay - head) * (ay + head);
  const double ratio = ay <= kMiddleBound ? middle : far;
  const double upper = math::exp(-head * head) * math::exp(-rest) * ratio;

  const double tail = y < 0.0 ? 2.0 - upper : upper;
  const double erfc = ay <= kSmallBound ? small : tail;
  return is_nan ? math::detail::kNaN : 0.5 * erfc;
}

}  // namespace aadp::pricer

#endif  // AADP_PRICER_NORMAL_CDF_HPP_
