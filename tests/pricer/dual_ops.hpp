// The pricer's Real operations for the forward-mode Dual of tests/support/, by ADL. Values come
// from the pricer's own functions, so a Dual kernel's value is the double kernel's bits; tangents
// multiply by the exact derivative evaluated at those values.
#ifndef AADP_TESTS_PRICER_DUAL_OPS_HPP_
#define AADP_TESTS_PRICER_DUAL_OPS_HPP_

#include <array>
#include <cstddef>
#include <numbers>

#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "pricer/normal_cdf.hpp"
#include "support/dual.hpp"

namespace aadp::test {

namespace pricer_dual_detail {

inline Dual Chain(double value, const Dual& x, double local) {
  Dual out(value);
  for (std::size_t i = 0; i < kTangents; ++i) {
    out.tangent[i] = x.tangent[i] == 0.0 ? 0.0 : x.tangent[i] * local;
  }
  return out;
}

inline double Pdf(double x) {
  return math::exp(-0.5 * x * x) * (std::numbers::inv_sqrtpi / std::numbers::sqrt2);
}

}  // namespace pricer_dual_detail

inline Dual Exp(const Dual& x) {
  const double value = math::exp(x.value);
  return pricer_dual_detail::Chain(value, x, value);
}
inline Dual Log(const Dual& x) {
  return pricer_dual_detail::Chain(math::log(x.value), x, 1.0 / x.value);
}
inline Dual Sqrt(const Dual& x) {
  const double value = math::sqrt(x.value);
  return pricer_dual_detail::Chain(value, x, 0.5 / value);
}
inline Dual InverseNormal(const Dual& p) {
  const double value = math::inverse_normal(p.value);
  return pricer_dual_detail::Chain(value, p, 1.0 / pricer_dual_detail::Pdf(value));
}
inline Dual NormalCdf(const Dual& x) {
  return pricer_dual_detail::Chain(pricer::NormalCdf(x.value), x, pricer_dual_detail::Pdf(x.value));
}
inline double Value(const Dual& x) { return x.value; }

}  // namespace aadp::test

#endif  // AADP_TESTS_PRICER_DUAL_OPS_HPP_
