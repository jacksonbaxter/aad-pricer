// The math kernels of src/math/ and the normal CDF of src/pricer/normal_cdf.hpp on Highway lanes.
//
// Each function is its scalar twin written lane by lane: the same operations on the same operands
// in the same order, with every `c ? a : b` an IfThenElse and every comparison the same IEEE
// predicate. Under the rules of src/math/common.hpp (no fused multiply-add, round to nearest,
// flush-to-zero and denormals-are-zero off) every lane then gets the bits the scalar kernel gets,
// NaN included, for any lane count. tests/simd/math_agreement_test.cpp checks that on every width
// the build can run, and fuzz/fuzz_simd_kernels.cpp on arbitrary inputs.
//
// What lane by lane needs beyond the obvious:
// - x != x is hn::IsNaN, never hn::Ne: Highway's Ne on x86 is the ordered not-equal
//   (_CMP_NEQ_OQ), which is false for NaN, while C++ != is true. On NEON the two agree, so only
//   an x86 build would show the difference.
// - No hn::Min, hn::Max or hn::Abs where the scalar code writes a compare and select: Highway
//   leaves their NaN result unspecified. Each is the scalar's own IfThenElse.
// - Integer to double is exact in the scalar code (|k| < 2^52), so here too, by adding the
//   integer to the bits of a power of two and subtracting it, which gives the same double.
// - No hn::MulAdd anywhere. The coefficients are the scalar headers' own arrays, so they cannot
//   drift apart.
//
// Branch-free: no lane, and no vector, takes a branch on its values. So a vector tape (stage 5b)
// can record these as they stand.
#ifndef AADP_SIMD_LANE_MATH_HPP_
#define AADP_SIMD_LANE_MATH_HPP_

#include <hwy/highway.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <numbers>

#include "math/common.hpp"
#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "pricer/normal_cdf.hpp"

HWY_BEFORE_NAMESPACE();

namespace aadp::simd::vmath {

namespace hn = hwy::HWY_NAMESPACE;

namespace detail {

// 1.5 * 2^52: an integer of magnitude below 2^51 added to its bits gives a double whose value is
// 1.5 * 2^52 plus the integer, exactly.
inline constexpr double kOneAndHalfTwo52 = 0x1.8p52;

// c[0] + t (c[1] + t (c[2] + ...)), as math::detail::Horner.
template <class D, std::size_t N>
hn::Vec<D> Horner(D d, const std::array<double, N>& c, hn::Vec<D> t) {
  hn::Vec<D> sum = hn::Set(d, c[N - 1]);
  for (std::size_t i = N - 1; i-- > 0;) sum = hn::Add(hn::Mul(sum, t), hn::Set(d, c[i]));
  return sum;
}

// static_cast<double>(k) for |k| < 2^51, exactly.
template <class D>
hn::Vec<D> SmallIntToDouble(D d, hn::Vec<hn::RebindToSigned<D>> k) {
  const hn::RebindToSigned<D> di;
  const auto bias = hn::Set(di, std::bit_cast<std::int64_t>(kOneAndHalfTwo52));
  return hn::Sub(hn::BitCast(d, hn::Add(k, bias)), hn::Set(d, kOneAndHalfTwo52));
}

}  // namespace detail

// math::exp, lane by lane.
template <class D>
hn::Vec<D> Exp(D d, hn::Vec<D> x) {
  using math::exp_detail::kExpPoly;
  using V = hn::Vec<D>;
  const hn::RebindToSigned<D> di;
  const hn::RebindToUnsigned<D> du;
  const auto set = [d](double value) { return hn::Set(d, value); };
  const auto is_nan = hn::IsNaN(x);
  const V lowest = set(math::exp_detail::kMinArgument);
  const V highest = set(math::exp_detail::kMaxArgument);
  V xc = hn::IfThenElse(hn::Lt(x, lowest), lowest, x);
  xc = hn::IfThenElse(hn::Gt(xc, highest), highest, xc);
  xc = hn::IfThenElse(is_nan, hn::Zero(d), xc);

  const V shifter = set(math::detail::kShifter);
  const V shifted = hn::Add(hn::Mul(xc, set(math::detail::kInvLn2)), shifter);
  const V kd = hn::Sub(shifted, shifter);
  const auto k = hn::Sub(hn::BitCast(di, shifted), hn::BitCast(di, shifter));

  const V r_hi = hn::Sub(xc, hn::Mul(kd, set(math::detail::kLn2Hi)));
  const V p = hn::Mul(kd, set(math::detail::kLn2Lo));
  const V r = hn::Sub(r_hi, p);
  const V r_err = hn::Sub(hn::Sub(r_hi, r), p);

  const V one = set(1.0);
  const V s = hn::Add(one, r);
  const V s_err = hn::Add(hn::Sub(one, s), r);
  const V w = hn::Add(hn::Mul(hn::Mul(r, r), detail::Horner(d, kExpPoly, r)), hn::Mul(r_err, s));
  const V m = hn::Add(s, hn::Add(s_err, w));

  // k / 2 rounded towards zero, as the scalar's integer division: add one to a negative k first.
  const auto negative = hn::BitCast(di, hn::ShiftRight<63>(hn::BitCast(du, k)));
  const auto k1 = hn::ShiftRight<1>(hn::Add(k, negative));
  const auto k2 = hn::Sub(k, k1);
  const auto bias = hn::Set(di, math::detail::kExponentBias);
  const V scale1 = hn::BitCast(d, hn::ShiftLeft<52>(hn::Add(k1, bias)));
  const V scale2 = hn::BitCast(d, hn::ShiftLeft<52>(hn::Add(k2, bias)));
  const V result = hn::Mul(hn::Mul(m, scale1), scale2);
  return hn::IfThenElse(is_nan, set(math::detail::kNaN), result);
}

// math::log, lane by lane.
template <class D>
hn::Vec<D> Log(D d, hn::Vec<D> x) {
  using math::log_detail::kLogPoly;
  using V = hn::Vec<D>;
  const hn::RebindToSigned<D> di;
  const hn::RebindToUnsigned<D> du;
  const auto set = [d](double value) { return hn::Set(d, value); };
  const V zero = hn::Zero(d);
  const V inf = set(math::detail::kInf);
  const V one = set(1.0);
  const auto finite_positive = hn::And(hn::Gt(x, zero), hn::Lt(x, inf));
  const V xv = hn::IfThenElse(finite_positive, x, one);
  const auto subnormal = hn::Lt(xv, set(math::log_detail::kSmallestNormal));
  const V xs = hn::IfThenElse(subnormal, hn::Mul(xv, set(math::log_detail::kSubnormalScale)), xv);

  const auto bits = hn::BitCast(du, xs);
  const V m0 = hn::BitCast(d, hn::Or(hn::And(bits, hn::Set(du, math::detail::kMantissaMask)),
                                     hn::Set(du, math::detail::kExponentOfOne)));
  const auto high = hn::Ge(m0, set(math::detail::kSqrt2));
  const V m = hn::IfThenElse(high, hn::Mul(set(0.5), m0), m0);
  auto e_int =
      hn::Sub(hn::BitCast(di, hn::ShiftRight<52>(bits)), hn::Set(di, math::detail::kExponentBias));
  e_int = hn::Sub(e_int, hn::IfThenElseZero(hn::RebindMask(di, subnormal),
                                            hn::Set(di, math::log_detail::kSubnormalShift)));
  e_int = hn::Add(e_int, hn::IfThenElseZero(hn::RebindMask(di, high), hn::Set(di, 1)));
  const V e = detail::SmallIntToDouble(d, e_int);

  const V f = hn::Sub(m, one);
  const V s = hn::Div(f, hn::Add(set(2.0), f));
  const V z = hn::Mul(s, s);
  const V half_f_squared = hn::Mul(hn::Mul(set(0.5), f), f);
  const V correction =
      hn::Sub(hn::Mul(s, hn::Add(half_f_squared, hn::Mul(z, detail::Horner(d, kLogPoly, z)))),
              half_f_squared);

  const V a = hn::Mul(e, set(math::detail::kLn2Hi));
  const V sum = hn::Add(a, f);
  const V b = hn::Sub(sum, a);
  const V sum_err = hn::Add(hn::Sub(a, hn::Sub(sum, b)), hn::Sub(f, b));
  V result =
      hn::Add(sum, hn::Add(sum_err, hn::Add(correction, hn::Mul(e, set(math::detail::kLn2Lo)))));

  result = hn::IfThenElse(hn::Eq(x, zero), hn::Neg(inf), result);
  result = hn::IfThenElse(hn::Eq(x, inf), inf, result);
  return hn::IfThenElse(hn::Or(hn::Lt(x, zero), hn::IsNaN(x)), set(math::detail::kNaN), result);
}

// math::sqrt, lane by lane: sanitise the argument, take the IEEE root, select the NaN.
//
// Not the scalar's shape, which sanitises with the same mask it selects with:
// IfThenElse(m, Sqrt(IfThenElse(m, x, 1)), NaN). At one lane on NEON (<1 x double>, LLVM 23,
// -O3) LLVM folded that to Sqrt(x): inside the true arm m holds, so the inner select is x, and
// Sqrt(x) is a NaN wherever m fails, so it dropped the outer select, and sqrt(-NaN) kept its sign
// and payload. Found by tests/simd/math_agreement_test.cpp. So the root is taken of
// |x| with x's sign put back, a real number for every negative x, which no fold can treat as the
// NaN it replaces. On the domain x >= 0 it is sqrt(x) bit for bit: Abs and CopySign only move the
// sign bit, the root of |x| is the IEEE root, and sqrt(-0) = -0 = CopySign(sqrt(+0), -0).
template <class D>
hn::Vec<D> Sqrt(D d, hn::Vec<D> x) {
  const auto in_domain = hn::Ge(x, hn::Zero(d));
  const hn::Vec<D> root = hn::CopySign(hn::Sqrt(hn::Abs(x)), x);
  return hn::IfThenElse(in_domain, root, hn::Set(d, math::detail::kNaN));
}

// math::inverse_normal, lane by lane: AS241 in its select form, every lane taking the log and
// sqrt of the tail. Skipping them when no lane is in a tail would change no value, but would put
// a branch on the values into the vector code, so it is left to a measurement (stage 11).
template <class D>
hn::Vec<D> InverseNormal(D d, hn::Vec<D> p) {
  using namespace math::inverse_normal_detail;
  using V = hn::Vec<D>;
  const auto set = [d](double value) { return hn::Set(d, value); };
  const V zero = hn::Zero(d);
  const V q = hn::Sub(p, set(0.5));
  const auto lower = hn::Lt(q, zero);
  const auto central = hn::Le(hn::IfThenElse(lower, hn::Neg(q), q), set(kCentralBound));
  const V tail = hn::IfThenElse(lower, p, hn::Sub(set(1.0), p));
  const auto in_tail = hn::AndNot(central, hn::Gt(tail, zero));

  const V r = Sqrt(d, hn::Neg(Log(d, hn::IfThenElse(in_tail, tail, set(0.5)))));
  const auto far = hn::Gt(r, set(kFarBound));
  const V tail_t = hn::IfThenElse(far, hn::Sub(r, set(kFarBound)), hn::Sub(r, set(kMiddleShift)));
  const V t = hn::IfThenElse(central, hn::Sub(set(kCentralShift), hn::Mul(q, q)), tail_t);

  const auto pick = [&](const std::array<double, 8>& c, const std::array<double, 8>& m,
                        const std::array<double, 8>& f, std::size_t i) {
    const V tail_coefficient = hn::IfThenElse(far, set(f[i]), set(m[i]));
    return hn::IfThenElse(central, set(c[i]), tail_coefficient);
  };
  V num = pick(kCentralNum, kMiddleNum, kTailNum, 7);
  V den = pick(kCentralDen, kMiddleDen, kTailDen, 7);
  for (std::size_t i = 7; i-- > 0;) {
    num = hn::Add(hn::Mul(num, t), pick(kCentralNum, kMiddleNum, kTailNum, i));
    den = hn::Add(hn::Mul(den, t), pick(kCentralDen, kMiddleDen, kTailDen, i));
  }
  const V ratio = hn::Div(hn::IfThenElse(central, hn::Mul(q, num), num), den);

  V x = hn::IfThenElse(hn::AndNot(central, lower), hn::Neg(ratio), ratio);
  const V inf = set(math::detail::kInf);
  const V infinity = hn::IfThenElse(lower, hn::Neg(inf), inf);
  x = hn::IfThenElse(hn::AndNot(central, hn::Eq(tail, zero)), infinity, x);
  const auto in_domain = hn::And(hn::Ge(p, zero), hn::Le(p, set(1.0)));
  return hn::IfThenElse(in_domain, x, set(math::detail::kNaN));
}

namespace detail {

// pricer::normal_cdf_detail::Rational, lane by lane.
template <class D, std::size_t N, std::size_t M>
hn::Vec<D> Rational(D d, const std::array<double, N>& num_c, const std::array<double, M>& den_c,
                    hn::Vec<D> t) {
  static_assert(N == M + 1);
  hn::Vec<D> num = hn::Mul(hn::Set(d, num_c[N - 1]), t);
  hn::Vec<D> den = t;
  for (std::size_t i = 0; i + 2 < N; ++i) {
    num = hn::Mul(hn::Add(num, hn::Set(d, num_c[i])), t);
    den = hn::Mul(hn::Add(den, hn::Set(d, den_c[i])), t);
  }
  return hn::Div(hn::Add(num, hn::Set(d, num_c[N - 2])), hn::Add(den, hn::Set(d, den_c[M - 1])));
}

}  // namespace detail

// pricer::NormalCdf, lane by lane, on the lane Exp.
template <class D>
hn::Vec<D> NormalCdf(D d, hn::Vec<D> x) {
  using namespace pricer::normal_cdf_detail;
  using V = hn::Vec<D>;
  const auto set = [d](double value) { return hn::Set(d, value); };
  const V zero = hn::Zero(d);
  const auto is_nan = hn::IsNaN(x);
  const V y = hn::Div(hn::Neg(hn::IfThenElse(is_nan, zero, x)), set(std::numbers::sqrt2));
  const V magnitude = hn::IfThenElse(hn::Lt(y, zero), hn::Neg(y), y);
  const V zero_bound = set(kZeroBound);
  const V ay = hn::IfThenElse(hn::Lt(magnitude, zero_bound), magnitude, zero_bound);

  const V small_bound = set(kSmallBound);
  const auto is_small = hn::Le(ay, small_bound);
  const V small_y = hn::IfThenElse(is_small, y, small_bound);
  const V small_t = hn::Mul(small_y, small_y);
  const V small =
      hn::Sub(set(1.0), hn::Mul(small_y, detail::Rational(d, kSmallNum, kSmallDen, small_t)));

  const V middle_bound = set(kMiddleBound);
  const V middle_y = hn::IfThenElse(hn::Lt(ay, middle_bound), ay, middle_bound);
  const V middle = detail::Rational(d, kMiddleNum, kMiddleDen, middle_y);
  const V far_y = hn::IfThenElse(hn::Gt(ay, middle_bound), ay, middle_bound);
  const V far_t = hn::Div(set(1.0), hn::Mul(far_y, far_y));
  const V far = hn::Div(
      hn::Sub(set(kInvSqrtPi), hn::Mul(far_t, detail::Rational(d, kFarNum, kFarDen, far_t))),
      far_y);
  const V sixteen = set(16.0);
  const V head = hn::Div(hn::Trunc(hn::Mul(ay, sixteen)), sixteen);
  const V rest = hn::Mul(hn::Sub(ay, head), hn::Add(ay, head));
  const V ratio = hn::IfThenElse(hn::Le(ay, middle_bound), middle, far);
  const V upper =
      hn::Mul(hn::Mul(Exp(d, hn::Mul(hn::Neg(head), head)), Exp(d, hn::Neg(rest))), ratio);

  const V tail = hn::IfThenElse(hn::Lt(y, zero), hn::Sub(set(2.0), upper), upper);
  const V erfc = hn::IfThenElse(is_small, small, tail);
  return hn::IfThenElse(is_nan, set(math::detail::kNaN), hn::Mul(set(0.5), erfc));
}

}  // namespace aadp::simd::vmath

HWY_AFTER_NAMESPACE();

#endif  // AADP_SIMD_LANE_MATH_HPP_
