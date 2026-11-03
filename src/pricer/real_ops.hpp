// The operations a pricer kernel may apply to a Real, and their double versions.
//
// Kernels call these unqualified, so another Real (the tape type, a forward-mode Dual, a SIMD
// lane type) supplies its own by argument-dependent lookup. The double versions route every
// transcendental through src/math/ and normal_cdf.hpp, so no libm result reaches a price.
//
// Branch-free rule. A kernel never branches on a Real. A comparison of Reals gives a mask, bool
// for double, and the only thing a kernel does with a mask is Select. A type whose comparisons
// return something other than bool (a SIMD mask, the error tracker in tests/pricer/) cannot
// compile a kernel that breaks this rule. Branches on the contract (product, direction, option
// type) are fine: they are the same for every path and every lane.
//
// A Real type must provide: + - * / with Real and double, unary -, comparisons with Real and
// double, construction from double, and Exp, Log, Sqrt, InverseNormal, NormalCdf, Select and
// Value. Max and Min below are written once on Select.
#ifndef AADP_PRICER_REAL_OPS_HPP_
#define AADP_PRICER_REAL_OPS_HPP_

#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "pricer/normal_cdf.hpp"

namespace aadp::pricer {

inline double Exp(double x) { return math::exp(x); }
inline double Log(double x) { return math::log(x); }
inline double Sqrt(double x) { return math::sqrt(x); }
inline double InverseNormal(double p) { return math::inverse_normal(p); }
// NormalCdf(double) is in normal_cdf.hpp.

// The plain value, for quantities that must not carry a derivative, such as a smoothing width.
inline double Value(double x) { return x; }

// Mask is bool here. Any Real whose comparisons also give bool, such as the Dual, uses this.
template <class Real>
Real Select(bool mask, const Real& if_true, const Real& if_false) {
  return mask ? if_true : if_false;
}

// std::max and std::min rules, including which argument a NaN falls through to.
template <class Real>
Real Max(const Real& a, const Real& b) {
  return Select(a < b, b, a);
}
template <class Real>
Real Min(const Real& a, const Real& b) {
  return Select(b < a, b, a);
}

}  // namespace aadp::pricer

#endif  // AADP_PRICER_REAL_OPS_HPP_
