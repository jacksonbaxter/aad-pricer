// The operations of LaneTapeReal, everything src/pricer/real_ops.hpp asks of a Real, found by
// argument-dependent lookup.
//
// Each is src/ad/real.hpp's operation lane by lane: the values are lane_real.hpp's, so each lane
// has the scalar's bits (stage 5a), and the partials are real.hpp's formulas on them, in its
// order, with no MulAdd. An operand passive in every lane is not recorded, as there; one active in
// some lanes is recorded with its lane mask, and the result is active in the union of its
// operands' lanes. Comparisons give an hn::Mask, so a kernel can only Select on them.
//
// Select returns the chosen operand itself and records nothing, as real.hpp's does. Each lane
// takes the side its mask picks, and is active where that side is. Where some lanes take one
// active statement and others a different one, no single index can stand for the result: the
// group is marked divergent and its caller falls back to the scalar tape.
#ifndef AADP_SIMD_LANE_TAPE_OPS_HPP_
#define AADP_SIMD_LANE_TAPE_OPS_HPP_

#include <hwy/highway.h>

#include <concepts>
#include <cstdint>
#include <numbers>
#include <type_traits>

#include "simd/lane_math.hpp"
#include "simd/lane_tape.hpp"

HWY_BEFORE_NAMESPACE();

namespace aadp::simd {

namespace hn = hwy::HWY_NAMESPACE;

namespace lane_tape_detail {

template <class D>
LaneTapeReal<D> Record(hn::Vec<D> value, const LaneTapeReal<D>& a, hn::Vec<D> da) {
  if (!a.active()) return LaneTapeReal<D>(value);
  return ActiveLaneTape<D>().Push(value, a.index(), a.lanes(), da);
}

template <class D>
LaneTapeReal<D> Record(hn::Vec<D> value, const LaneTapeReal<D>& a, hn::Vec<D> da,
                       const LaneTapeReal<D>& b, hn::Vec<D> db) {
  if (a.active()) {
    if (b.active()) {
      return ActiveLaneTape<D>().Push(value, a.index(), a.lanes(), da, b.index(), b.lanes(), db);
    }
    return ActiveLaneTape<D>().Push(value, a.index(), a.lanes(), da);
  }
  if (b.active()) return ActiveLaneTape<D>().Push(value, b.index(), b.lanes(), db);
  return LaneTapeReal<D>(value);
}

template <class D>
hn::Vec<D> One() {
  return hn::Set(D(), 1.0);
}

// real.hpp's NormalPdf: exp(-0.5 x x) / sqrt(2 pi), in that order.
template <class D>
hn::Vec<D> NormalPdf(hn::Vec<D> x) {
  const D d;
  return hn::Mul(vmath::Exp(d, hn::Mul(hn::Mul(hn::Set(d, -0.5), x), x)),
                 hn::Set(d, std::numbers::inv_sqrtpi / std::numbers::sqrt2));
}

template <class D>
LaneBits BitsOf(const hn::Mask<D>& mask) {
  return static_cast<LaneBits>(hn::BitsFromMask(D(), mask));
}

template <class T>
struct TapeTagOf {};
template <class D>
struct TapeTagOf<LaneTapeReal<D>> {
  using type = D;
};

// One LaneTapeReal and a double, in either order, for the comparisons.
template <class A, class B>
concept MixedOperands = (requires { typename TapeTagOf<A>::type; } && std::same_as<B, double>) ||
                        (std::same_as<A, double> && requires { typename TapeTagOf<B>::type; });

template <class A, class B>
using MixedTag = TapeTagOf<std::conditional_t<std::same_as<A, double>, B, A>>::type;

template <class D>
hn::Vec<D> ValuesOf(const LaneTapeReal<D>& x) {
  return x.vec();
}
template <class D>
hn::Vec<D> ValuesOf(double x) {
  return hn::Set(D(), x);
}

}  // namespace lane_tape_detail

template <class D>
LaneTapeReal<D> operator+(const LaneTapeReal<D>& a) {
  return a;
}
template <class D>
LaneTapeReal<D> operator-(const LaneTapeReal<D>& a) {
  return lane_tape_detail::Record(hn::Neg(a.vec()), a, hn::Set(D(), -1.0));
}

template <class D>
LaneTapeReal<D> operator+(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  using namespace lane_tape_detail;
  return Record(hn::Add(a.vec(), b.vec()), a, One<D>(), b, One<D>());
}
template <class D>
LaneTapeReal<D> operator+(const LaneTapeReal<D>& a, double b) {
  using namespace lane_tape_detail;
  return Record(hn::Add(a.vec(), hn::Set(D(), b)), a, One<D>());
}
template <class D>
LaneTapeReal<D> operator+(double a, const LaneTapeReal<D>& b) {
  using namespace lane_tape_detail;
  return Record(hn::Add(hn::Set(D(), a), b.vec()), b, One<D>());
}

template <class D>
LaneTapeReal<D> operator-(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  using namespace lane_tape_detail;
  return Record(hn::Sub(a.vec(), b.vec()), a, One<D>(), b, hn::Set(D(), -1.0));
}
template <class D>
LaneTapeReal<D> operator-(const LaneTapeReal<D>& a, double b) {
  using namespace lane_tape_detail;
  return Record(hn::Sub(a.vec(), hn::Set(D(), b)), a, One<D>());
}
template <class D>
LaneTapeReal<D> operator-(double a, const LaneTapeReal<D>& b) {
  using namespace lane_tape_detail;
  return Record(hn::Sub(hn::Set(D(), a), b.vec()), b, hn::Set(D(), -1.0));
}

template <class D>
LaneTapeReal<D> operator*(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  using namespace lane_tape_detail;
  return Record(hn::Mul(a.vec(), b.vec()), a, b.vec(), b, a.vec());
}
template <class D>
LaneTapeReal<D> operator*(const LaneTapeReal<D>& a, double b) {
  using namespace lane_tape_detail;
  const hn::Vec<D> vb = hn::Set(D(), b);
  return Record(hn::Mul(a.vec(), vb), a, vb);
}
template <class D>
LaneTapeReal<D> operator*(double a, const LaneTapeReal<D>& b) {
  using namespace lane_tape_detail;
  const hn::Vec<D> va = hn::Set(D(), a);
  return Record(hn::Mul(va, b.vec()), b, va);
}

template <class D>
LaneTapeReal<D> operator/(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  using namespace lane_tape_detail;
  const hn::Vec<D> value = hn::Div(a.vec(), b.vec());
  if (!a.active() && !b.active()) return LaneTapeReal<D>(value);
  return Record(value, a, hn::Div(One<D>(), b.vec()), b,
                hn::Div(hn::Neg(a.vec()), hn::Mul(b.vec(), b.vec())));
}
template <class D>
LaneTapeReal<D> operator/(const LaneTapeReal<D>& a, double b) {
  using namespace lane_tape_detail;
  const hn::Vec<D> vb = hn::Set(D(), b);
  const hn::Vec<D> value = hn::Div(a.vec(), vb);
  if (!a.active()) return LaneTapeReal<D>(value);
  return Record(value, a, hn::Div(One<D>(), vb));
}
template <class D>
LaneTapeReal<D> operator/(double a, const LaneTapeReal<D>& b) {
  using namespace lane_tape_detail;
  const hn::Vec<D> va = hn::Set(D(), a);
  const hn::Vec<D> value = hn::Div(va, b.vec());
  if (!b.active()) return LaneTapeReal<D>(value);
  return Record(value, b, hn::Div(hn::Neg(va), hn::Mul(b.vec(), b.vec())));
}

template <class D>
LaneTapeReal<D>& operator+=(LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return a = a + b;
}
template <class D>
LaneTapeReal<D>& operator-=(LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return a = a - b;
}
template <class D>
LaneTapeReal<D>& operator*=(LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return a = a * b;
}
template <class D>
LaneTapeReal<D>& operator/=(LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return a = a / b;
}

// Comparisons read values only, each with its C++ predicate; != is not-equal, true for NaN
// (lane_real.hpp says why it is not hn::Ne).
template <class D>
hn::Mask<D> operator<(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return hn::Lt(a.vec(), b.vec());
}
template <class D>
hn::Mask<D> operator<=(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return hn::Le(a.vec(), b.vec());
}
template <class D>
hn::Mask<D> operator>(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return hn::Gt(a.vec(), b.vec());
}
template <class D>
hn::Mask<D> operator>=(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return hn::Ge(a.vec(), b.vec());
}
template <class D>
hn::Mask<D> operator==(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return hn::Eq(a.vec(), b.vec());
}
template <class D>
hn::Mask<D> operator!=(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return hn::Not(hn::Eq(a.vec(), b.vec()));
}

template <class A, class B>
  requires lane_tape_detail::MixedOperands<A, B>
auto operator<(const A& a, const B& b) {
  using D = lane_tape_detail::MixedTag<A, B>;
  return hn::Lt(lane_tape_detail::ValuesOf<D>(a), lane_tape_detail::ValuesOf<D>(b));
}
template <class A, class B>
  requires lane_tape_detail::MixedOperands<A, B>
auto operator<=(const A& a, const B& b) {
  using D = lane_tape_detail::MixedTag<A, B>;
  return hn::Le(lane_tape_detail::ValuesOf<D>(a), lane_tape_detail::ValuesOf<D>(b));
}
template <class A, class B>
  requires lane_tape_detail::MixedOperands<A, B>
auto operator>(const A& a, const B& b) {
  using D = lane_tape_detail::MixedTag<A, B>;
  return hn::Gt(lane_tape_detail::ValuesOf<D>(a), lane_tape_detail::ValuesOf<D>(b));
}
template <class A, class B>
  requires lane_tape_detail::MixedOperands<A, B>
auto operator>=(const A& a, const B& b) {
  using D = lane_tape_detail::MixedTag<A, B>;
  return hn::Ge(lane_tape_detail::ValuesOf<D>(a), lane_tape_detail::ValuesOf<D>(b));
}

template <class D>
LaneTapeReal<D> Exp(const LaneTapeReal<D>& x) {
  const hn::Vec<D> value = vmath::Exp(D(), x.vec());
  return lane_tape_detail::Record(value, x, value);
}

template <class D>
LaneTapeReal<D> Log(const LaneTapeReal<D>& x) {
  const hn::Vec<D> value = vmath::Log(D(), x.vec());
  if (!x.active()) return LaneTapeReal<D>(value);
  return lane_tape_detail::Record(value, x, hn::Div(lane_tape_detail::One<D>(), x.vec()));
}

template <class D>
LaneTapeReal<D> Sqrt(const LaneTapeReal<D>& x) {
  const hn::Vec<D> value = vmath::Sqrt(D(), x.vec());
  if (!x.active()) return LaneTapeReal<D>(value);
  return lane_tape_detail::Record(value, x, hn::Div(hn::Set(D(), 0.5), value));
}

template <class D>
LaneTapeReal<D> NormalCdf(const LaneTapeReal<D>& x) {
  const hn::Vec<D> value = vmath::NormalCdf(D(), x.vec());
  if (!x.active()) return LaneTapeReal<D>(value);
  return lane_tape_detail::Record(value, x, lane_tape_detail::NormalPdf<D>(x.vec()));
}

template <class D>
LaneTapeReal<D> InverseNormal(const LaneTapeReal<D>& p) {
  const hn::Vec<D> value = vmath::InverseNormal(D(), p.vec());
  if (!p.active()) return LaneTapeReal<D>(value);
  return lane_tape_detail::Record(
      value, p, hn::Div(lane_tape_detail::One<D>(), lane_tape_detail::NormalPdf<D>(value)));
}

template <class D>
LaneTapeReal<D> Select(const hn::Mask<D>& mask, const LaneTapeReal<D>& if_true,
                       const LaneTapeReal<D>& if_false) {
  const hn::Vec<D> value = hn::IfThenElse(mask, if_true.vec(), if_false.vec());
  const LaneBits chosen = lane_tape_detail::BitsOf<D>(mask);
  const auto from_true = static_cast<LaneBits>(chosen & if_true.lanes());
  const auto from_false = static_cast<LaneBits>(~static_cast<unsigned>(chosen) & if_false.lanes());
  if (from_false == 0) return LaneTapeReal<D>::Active(value, if_true.index(), from_true);
  if (from_true == 0) return LaneTapeReal<D>::Active(value, if_false.index(), from_false);
  if (if_true.index() != if_false.index()) ActiveLaneTape<D>().MarkDivergent();
  return LaneTapeReal<D>::Active(value, if_true.index(),
                                 static_cast<LaneBits>(from_true | from_false));
}

// The plain lane values, carrying no derivative.
template <class D>
LaneTapeReal<D> Value(const LaneTapeReal<D>& x) {
  return LaneTapeReal<D>(x.vec());
}

// real_ops.hpp's rules, on Select, so a NaN falls through to the same argument as there.
template <class D>
LaneTapeReal<D> Max(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return Select(a < b, b, a);
}
template <class D>
LaneTapeReal<D> Min(const LaneTapeReal<D>& a, const LaneTapeReal<D>& b) {
  return Select(b < a, b, a);
}

}  // namespace aadp::simd

HWY_AFTER_NAMESPACE();

#endif  // AADP_SIMD_LANE_TAPE_OPS_HPP_
