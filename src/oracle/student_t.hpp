// Student t distribution from the regularised incomplete beta function. Computed, never typed,
// so a change in K or B changes the critical value by itself.
#ifndef AADP_ORACLE_STUDENT_T_HPP_
#define AADP_ORACLE_STUDENT_T_HPP_

#include <cmath>
#include <limits>

namespace aadp {

namespace student_t_detail {

// Continued fraction for I_x(a, b), modified Lentz (DLMF 8.17.22). Converges fast for
// x < (a + 1) / (a + b + 2), which the caller arranges.
inline double BetaContinuedFraction(double x, double a, double b) {
  constexpr double kTiny = 1e-300;
  constexpr double kEpsilon = 1e-16;
  const auto guard = [](double v) { return std::fabs(v) < kTiny ? kTiny : v; };
  double c = 1.0;
  double d = 1.0 / guard(1.0 - (a + b) * x / (a + 1.0));
  double result = d;
  for (int m = 1; m <= 2000; ++m) {
    const double dm = m;
    const double even = dm * (b - dm) * x / ((a + 2.0 * dm - 1.0) * (a + 2.0 * dm));
    d = 1.0 / guard(1.0 + even * d);
    c = guard(1.0 + even / c);
    result *= d * c;
    const double odd = -(a + dm) * (a + b + dm) * x / ((a + 2.0 * dm) * (a + 2.0 * dm + 1.0));
    d = 1.0 / guard(1.0 + odd * d);
    c = guard(1.0 + odd / c);
    const double delta = d * c;
    result *= delta;
    if (std::fabs(delta - 1.0) < kEpsilon) break;
  }
  return result;
}

}  // namespace student_t_detail

inline double RegularizedIncompleteBeta(double x, double a, double b) {
  if (!(x >= 0.0 && x <= 1.0) || !(a > 0.0) || !(b > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (x == 0.0 || x == 1.0) return x;
  // NOLINTBEGIN(concurrency-mt-unsafe): lgamma's only shared state is signgam, never read here.
  const double log_front =
      std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) + a * std::log(x) + b * std::log1p(-x);
  // NOLINTEND(concurrency-mt-unsafe)
  const double front = std::exp(log_front);
  if (x < (a + 1.0) / (a + b + 2.0)) {
    return front * student_t_detail::BetaContinuedFraction(x, a, b) / a;
  }
  return 1.0 - front * student_t_detail::BetaContinuedFraction(1.0 - x, b, a) / b;
}

// P(T > t) for t >= 0, kept as an upper tail so small probabilities keep their digits.
inline double StudentTUpperTail(double t, double nu) {
  return 0.5 * RegularizedIncompleteBeta(nu / (nu + t * t), 0.5 * nu, 0.5);
}

// t with P(T <= t) = p, for p in [1/2, 1). NaN outside that range, or for nu <= 0.
inline double StudentTQuantile(double p, double nu) {
  if (!(p >= 0.5 && p < 1.0) || !(nu > 0.0)) return std::numeric_limits<double>::quiet_NaN();
  const double upper = 1.0 - p;
  double lo = 0.0;
  double hi = 1.0;
  while (StudentTUpperTail(hi, nu) > upper) {
    lo = hi;
    hi *= 2.0;
    if (!std::isfinite(hi)) return std::numeric_limits<double>::infinity();
  }
  for (int i = 0; i < 400 && hi - lo > 1e-13 * hi; ++i) {
    const double mid = 0.5 * (lo + hi);
    (StudentTUpperTail(mid, nu) > upper ? lo : hi) = mid;
  }
  return 0.5 * (lo + hi);
}

}  // namespace aadp

#endif  // AADP_ORACLE_STUDENT_T_HPP_
