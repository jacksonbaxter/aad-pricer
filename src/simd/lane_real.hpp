// LaneReal<D>: one double per lane of the Highway tag D, as a Real for the lane kernels
// (lane_kernels.hpp). It supplies what src/pricer/real_ops.hpp asks of a Real: + - * / with
// LaneReal and double, unary -, comparisons, construction from double, and Exp, Log, Sqrt,
// InverseNormal, NormalCdf, Select and Value, found by argument-dependent lookup.
//
// A comparison gives an hn::Mask<D>, which has no conversion to bool, so a kernel that branches on
// a value does not compile: the only thing it can do with a mask is Select. Each comparison is the
// C++ operator's IEEE predicate: the ordered <, <=, >, >= and ==, and != as not-equal, true for
// NaN (Highway's own Ne is ordered on x86; lane_math.hpp says why that matters).
//
// The primal only. Value returns its argument, which is what it does for double. The vector tape
// of stage 5b brings its own type.
#ifndef AADP_SIMD_LANE_REAL_HPP_
#define AADP_SIMD_LANE_REAL_HPP_

#include <hwy/highway.h>

#include <concepts>
#include <type_traits>

#include "simd/lane_math.hpp"

HWY_BEFORE_NAMESPACE();

namespace aadp::simd {

namespace hn = hwy::HWY_NAMESPACE;

template <class D>
class LaneReal {
 public:
  using Tag = D;
  using Vec = hn::Vec<D>;

  LaneReal() : v_(hn::Zero(D())) {}
  explicit LaneReal(double x) : v_(hn::Set(D(), x)) {}
  explicit LaneReal(Vec v) : v_(v) {}

  [[nodiscard]] Vec vec() const { return v_; }

 private:
  Vec v_;
};

template <class D>
using LaneMask = hn::Mask<D>;

// Operands of a binary operation: two LaneReal of one tag, or one of them and a double. A double
// is broadcast, which is the scalar's own value in every lane. Nothing else converts, so an int
// operand does not compile rather than round somewhere unseen.
template <class T>
struct LaneTagOf {};
template <class D>
struct LaneTagOf<LaneReal<D>> {
  using type = D;
};

template <class A, class B>
concept LaneOperands = (requires { typename LaneTagOf<A>::type; } &&
                        (std::same_as<A, B> || std::same_as<B, double>)) ||
                       (std::same_as<A, double> && requires { typename LaneTagOf<B>::type; });

template <class A, class B>
using OperandTag = LaneTagOf<std::conditional_t<std::same_as<A, double>, B, A>>::type;

template <class D>
hn::Vec<D> AsVec(const LaneReal<D>& x) {
  return x.vec();
}
template <class D>
hn::Vec<D> AsVec(double x) {
  return hn::Set(D(), x);
}

template <class A, class B>
  requires LaneOperands<A, B>
auto operator+(const A& a, const B& b) {
  using D = OperandTag<A, B>;
  return LaneReal<D>(hn::Add(AsVec<D>(a), AsVec<D>(b)));
}
template <class A, class B>
  requires LaneOperands<A, B>
auto operator-(const A& a, const B& b) {
  using D = OperandTag<A, B>;
  return LaneReal<D>(hn::Sub(AsVec<D>(a), AsVec<D>(b)));
}
template <class A, class B>
  requires LaneOperands<A, B>
auto operator*(const A& a, const B& b) {
  using D = OperandTag<A, B>;
  return LaneReal<D>(hn::Mul(AsVec<D>(a), AsVec<D>(b)));
}
template <class A, class B>
  requires LaneOperands<A, B>
auto operator/(const A& a, const B& b) {
  using D = OperandTag<A, B>;
  return LaneReal<D>(hn::Div(AsVec<D>(a), AsVec<D>(b)));
}

template <class D>
LaneReal<D> operator-(const LaneReal<D>& a) {
  return LaneReal<D>(hn::Neg(a.vec()));
}

// Comparisons, each with its C++ predicate.
template <class A, class B>
  requires LaneOperands<A, B>
auto operator<(const A& a, const B& b) {
  using D = OperandTag<A, B>;
  return hn::Lt(AsVec<D>(a), AsVec<D>(b));
}
template <class A, class B>
  requires LaneOperands<A, B>
auto operator<=(const A& a, const B& b) {
  using D = OperandTag<A, B>;
  return hn::Le(AsVec<D>(a), AsVec<D>(b));
}
template <class A, class B>
  requires LaneOperands<A, B>
auto operator>(const A& a, const B& b) {
  using D = OperandTag<A, B>;
  return hn::Gt(AsVec<D>(a), AsVec<D>(b));
}
template <class A, class B>
  requires LaneOperands<A, B>
auto operator>=(const A& a, const B& b) {
  using D = OperandTag<A, B>;
  return hn::Ge(AsVec<D>(a), AsVec<D>(b));
}
template <class A, class B>
  requires LaneOperands<A, B>
auto operator==(const A& a, const B& b) {
  using D = OperandTag<A, B>;
  return hn::Eq(AsVec<D>(a), AsVec<D>(b));
}
// Not hn::Ne, which is the ordered not-equal on x86.
template <class A, class B>
  requires LaneOperands<A, B>
auto operator!=(const A& a, const B& b) {
  using D = OperandTag<A, B>;
  return hn::Not(hn::Eq(AsVec<D>(a), AsVec<D>(b)));
}

template <class D>
LaneReal<D> Exp(const LaneReal<D>& x) {
  return LaneReal<D>(vmath::Exp(D(), x.vec()));
}
template <class D>
LaneReal<D> Log(const LaneReal<D>& x) {
  return LaneReal<D>(vmath::Log(D(), x.vec()));
}
template <class D>
LaneReal<D> Sqrt(const LaneReal<D>& x) {
  return LaneReal<D>(vmath::Sqrt(D(), x.vec()));
}
template <class D>
LaneReal<D> InverseNormal(const LaneReal<D>& p) {
  return LaneReal<D>(vmath::InverseNormal(D(), p.vec()));
}
template <class D>
LaneReal<D> NormalCdf(const LaneReal<D>& x) {
  return LaneReal<D>(vmath::NormalCdf(D(), x.vec()));
}

template <class D>
LaneReal<D> Select(const LaneMask<D>& mask, const LaneReal<D>& if_true,
                   const LaneReal<D>& if_false) {
  return LaneReal<D>(hn::IfThenElse(mask, if_true.vec(), if_false.vec()));
}

template <class D>
LaneReal<D> Value(const LaneReal<D>& x) {
  return x;
}

// real_ops.hpp's rules, on Select, so a NaN falls through to the same argument as there.
template <class D>
LaneReal<D> Max(const LaneReal<D>& a, const LaneReal<D>& b) {
  return Select(a < b, b, a);
}
template <class D>
LaneReal<D> Min(const LaneReal<D>& a, const LaneReal<D>& b) {
  return Select(b < a, b, a);
}

}  // namespace aadp::simd

HWY_AFTER_NAMESPACE();

#endif  // AADP_SIMD_LANE_REAL_HPP_
