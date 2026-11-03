// Double-double exp and log, about 1e-30 relative, as the reference for dense sweeps that would
// make the committed table too large. Test code only. No FMA: products split with Veltkamp's
// constant (Dekker, Numer. Math. 18, 1971), exact under round to nearest away from overflow.
// Checked against the decimal table in exp_log_test.cpp before any sweep relies on it.
#ifndef AADP_TESTS_MATH_DOUBLE_DOUBLE_HPP_
#define AADP_TESTS_MATH_DOUBLE_DOUBLE_HPP_

#include <cmath>

#include "reference_table.hpp"

namespace aadp::math::testing {

struct DoubleDouble {
  constexpr DoubleDouble(double hi_value, double lo_value) : hi(hi_value), lo(lo_value) {}
  double hi;
  double lo;
};

inline DoubleDouble TwoSum(double a, double b) {
  const double s = a + b;
  const double bb = s - a;
  return {s, (a - (s - bb)) + (b - bb)};
}

inline DoubleDouble FastTwoSum(double a, double b) {
  const double s = a + b;
  return {s, b - (s - a)};
}

inline DoubleDouble Split(double a) {
  const double c = 134217729.0 * a;  // 2^27 + 1
  const double hi = c - (c - a);
  return {hi, a - hi};
}

inline DoubleDouble TwoProduct(double a, double b) {
  const double p = a * b;
  const DoubleDouble as = Split(a);
  const DoubleDouble bs = Split(b);
  const double err = ((as.hi * bs.hi - p) + as.hi * bs.lo + as.lo * bs.hi) + as.lo * bs.lo;
  return {p, err};
}

inline DoubleDouble Add(DoubleDouble a, DoubleDouble b) {
  DoubleDouble s = TwoSum(a.hi, b.hi);
  const DoubleDouble t = TwoSum(a.lo, b.lo);
  s = FastTwoSum(s.hi, s.lo + t.hi);
  return FastTwoSum(s.hi, s.lo + t.lo);
}

inline DoubleDouble Negate(DoubleDouble a) { return {-a.hi, -a.lo}; }

inline DoubleDouble Mul(DoubleDouble a, DoubleDouble b) {
  const DoubleDouble p = TwoProduct(a.hi, b.hi);
  return FastTwoSum(p.hi, p.lo + (a.hi * b.lo + a.lo * b.hi));
}

inline DoubleDouble Div(DoubleDouble a, DoubleDouble b) {
  const double q1 = a.hi / b.hi;
  DoubleDouble r = Add(a, Negate(Mul(b, {q1, 0.0})));
  const double q2 = r.hi / b.hi;
  r = Add(r, Negate(Mul(b, {q2, 0.0})));
  const double q3 = r.hi / b.hi;
  return Add(FastTwoSum(q1, q2), {q3, 0.0});
}

// k ln2 as a double-double, for integer k with |k| < 2^11. The first product is exact.
inline DoubleDouble KLn2(double k) {
  const DoubleDouble first = TwoProduct(k, kLn2Triple[0]);
  const DoubleDouble second = TwoProduct(k, kLn2Triple[1]);
  return Add(Add(first, second), {k * kLn2Triple[2], 0.0});
}

// exp(x) for -670 <= x < 709.78. Below that the lo part of the result is subnormal and loses
// bits, which once showed as a false 1.0 ULP error in the kernel at x = -707.9, so the decimal
// table covers that range alone. Reduced by k ln2 and then by 2^-8, Taylor to degree 14, squared
// eight times: the squaring multiplies the 2^-104 error by 2^8.
inline DoubleDouble ExpDD(double x) {
  const double k = std::nearbyint(x / kLn2Triple[0]);
  DoubleDouble r = Add({x, 0.0}, Negate(KLn2(k)));
  r = {std::ldexp(r.hi, -8), std::ldexp(r.lo, -8)};
  DoubleDouble term = r;
  DoubleDouble sum = Add({1.0, 0.0}, r);
  for (int n = 2; n <= 14; ++n) {
    term = Div(Mul(term, r), {static_cast<double>(n), 0.0});
    sum = Add(sum, term);
  }
  for (int i = 0; i < 8; ++i) sum = Mul(sum, sum);
  const int scale = static_cast<int>(k);
  return {std::ldexp(sum.hi, scale), std::ldexp(sum.lo, scale)};
}

// log(x) for finite x > 0, normal or subnormal: x = 2^e m with m in [sqrt(1/2), sqrt 2), then
// e ln2 + 2 atanh(s), s = (m - 1) / (m + 1), summed to 1e-36 relative.
inline DoubleDouble LogDD(double x) {
  int e = 0;
  double m = std::frexp(x, &e);
  if (m < 0.70710678118654752) {
    m *= 2.0;
    --e;
  }
  const double f = m - 1.0;
  const DoubleDouble s = Div({f, 0.0}, TwoSum(2.0, f));
  const DoubleDouble s2 = Mul(s, s);
  DoubleDouble term = s;
  DoubleDouble sum = s;
  for (int k = 1; k < 60; ++k) {
    term = Mul(term, s2);
    const DoubleDouble add = Div(term, {2.0 * k + 1.0, 0.0});
    sum = Add(sum, add);
    if (std::fabs(add.hi) < 1e-36 * std::fabs(s.hi)) break;
  }
  return Add({2.0 * sum.hi, 2.0 * sum.lo}, KLn2(static_cast<double>(e)));
}

// sqrt(x) for finite x > 0, one Newton step in double-double from the IEEE root.
inline DoubleDouble SqrtDD(double x) {
  const double y = std::sqrt(x);
  const DoubleDouble residual = Add({x, 0.0}, Negate(TwoProduct(y, y)));
  const double step = residual.hi / (2.0 * y);
  return FastTwoSum(y, step);
}

}  // namespace aadp::math::testing

#endif  // AADP_TESTS_MATH_DOUBLE_DOUBLE_HPP_
