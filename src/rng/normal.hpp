// Standard normal functions. Inverse: Wichura, Algorithm AS 241 (PPND16), Applied Statistics
// 37(3), 1988, coefficients from that paper.
//
// All are double overloads in namespace aadp. Templated kernels call them unqualified, so an
// active type supplies its own overload by argument-dependent lookup.
#ifndef AADP_RNG_NORMAL_HPP_
#define AADP_RNG_NORMAL_HPP_

#include <cmath>
#include <limits>
#include <numbers>

namespace aadp {

inline double normal_pdf(double x) {
  return std::exp(-0.5 * x * x) * 0.5 * std::numbers::sqrt2 * std::numbers::inv_sqrtpi;
}

inline double normal_cdf(double x) { return 0.5 * std::erfc(-x / std::numbers::sqrt2); }

namespace ppnd16_detail {

template <int N>
constexpr double Polynomial(const double (&c)[N], double x) {
  double sum = c[N - 1];
  for (int i = N - 2; i >= 0; --i) sum = sum * x + c[i];
  return sum;
}

inline constexpr double kCentralNum[] = {3.3871328727963666080e0,  1.3314166789178437745e+2,
                                         1.9715909503065514427e+3, 1.3731693765509461125e+4,
                                         4.5921953931549871457e+4, 6.7265770927008700853e+4,
                                         3.3430575583588128105e+4, 2.5090809287301226727e+3};
inline constexpr double kCentralDen[] = {1.0,
                                         4.2313330701600911252e+1,
                                         6.8718700749205790830e+2,
                                         5.3941960214247511077e+3,
                                         2.1213794301586595867e+4,
                                         3.9307895800092710610e+4,
                                         2.8729085735721942674e+4,
                                         5.2264952788528545610e+3};
inline constexpr double kMiddleNum[] = {1.42343711074968357734e0,  4.63033784615654529590e0,
                                        5.76949722146069140550e0,  3.64784832476320460504e0,
                                        1.27045825245236838258e0,  2.41780725177450611770e-1,
                                        2.27238449892691845833e-2, 7.74545014278341407640e-4};
inline constexpr double kMiddleDen[] = {1.0,
                                        2.05319162663775882187e0,
                                        1.67638483018380384940e0,
                                        6.89767334985100004550e-1,
                                        1.48103976427480074590e-1,
                                        1.51986665636164571966e-2,
                                        5.47593808499534494600e-4,
                                        1.05075007164441684324e-9};
inline constexpr double kTailNum[] = {6.65790464350110377720e0,  5.46378491116411436990e0,
                                      1.78482653991729133580e0,  2.96560571828504891230e-1,
                                      2.65321895265761230930e-2, 1.24266094738807843860e-3,
                                      2.71155556874348757815e-5, 2.01033439929228813265e-7};
inline constexpr double kTailDen[] = {1.0,
                                      5.99832206555887937690e-1,
                                      1.36929880922735805310e-1,
                                      1.48753612908506148525e-2,
                                      7.86869131145613259100e-4,
                                      1.84631831751005468180e-5,
                                      1.42151175831644588870e-7,
                                      2.04426310338993978564e-15};

}  // namespace ppnd16_detail

// NaN outside [0, 1] and for NaN, -inf at 0, +inf at 1.
inline double inverse_normal(double p) {
  using namespace ppnd16_detail;
  if (!(p >= 0.0 && p <= 1.0)) return std::numeric_limits<double>::quiet_NaN();
  const double q = p - 0.5;
  if (std::fabs(q) <= 0.425) {
    const double r = 0.180625 - q * q;
    return q * Polynomial(kCentralNum, r) / Polynomial(kCentralDen, r);
  }
  const double tail = q < 0.0 ? p : 1.0 - p;
  if (tail == 0.0) {
    return std::copysign(std::numeric_limits<double>::infinity(), q);
  }
  double r = std::sqrt(-std::log(tail));
  double x = 0.0;
  if (r <= 5.0) {
    r -= 1.6;
    x = Polynomial(kMiddleNum, r) / Polynomial(kMiddleDen, r);
  } else {
    r -= 5.0;
    x = Polynomial(kTailNum, r) / Polynomial(kTailDen, r);
  }
  return q < 0.0 ? -x : x;
}

}  // namespace aadp

#endif  // AADP_RNG_NORMAL_HPP_
