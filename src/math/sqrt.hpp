// Square root for double: std::sqrt is enough, and this wrapper only fixes the NaN it returns.
//
// IEEE 754 requires square root to be correctly rounded, like + - * /, so every conforming host
// and every SIMD square-root instruction (NEON fsqrt, SSE2 sqrtpd, AVX vsqrtpd) gives the same
// bits for the same input under round to nearest. Compilers emit that instruction for std::sqrt,
// and glibc's and Apple's sqrt are correctly rounded when it is called. So no kernel of our own
// could be more host independent, and none could be more accurate. The accuracy target is
// therefore exact: correctly rounded, 0.5 ULP, checked in tests/math/exp_log_test.cpp.
//
// The one thing hosts do differ in is the NaN for a negative argument: x86 returns the default
// NaN with its sign bit set, Arm a positive one. So every NaN result becomes the canonical quiet
// NaN, as in the other kernels. Edge contract: sqrt(+0) = +0, sqrt(-0) = -0, sqrt(+inf) = +inf,
// x < 0, -inf and NaN give the canonical quiet NaN.
//
// The argument is sanitised before the root rather than the NaN replaced after it. LLVM treats
// NaN sign and payload as unspecified, so it may fold `isnan(y) ? kNaN : y` to y, and in one
// inlining context of the hash test it did: sqrt(-NaN) kept its sign bit at -O3 and not at -O0.
// A select whose other operand is a finite dummy result cannot be folded that way.
#ifndef AADP_MATH_SQRT_HPP_
#define AADP_MATH_SQRT_HPP_

#include <cmath>

#include "math/common.hpp"

namespace aadp::math {

inline double sqrt(double x) {
  const bool in_domain = x >= 0.0;  // false for NaN, true for -0 and +inf
  const double root = std::sqrt(in_domain ? x : 1.0);
  return in_domain ? root : detail::kNaN;
}

}  // namespace aadp::math

#endif  // AADP_MATH_SQRT_HPP_
