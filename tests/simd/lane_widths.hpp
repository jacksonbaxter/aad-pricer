// Every lane width this build can run, for the SIMD agreement tests: f(FixedTag<double, L>()) for
// L = 1, 2, 4, 8 up to the static target's double lanes.
#ifndef AADP_TESTS_SIMD_LANE_WIDTHS_HPP_
#define AADP_TESTS_SIMD_LANE_WIDTHS_HPP_

#include <hwy/highway.h>

#include <bit>
#include <cstddef>
#include <cstdint>

namespace aadp::simd::testing {

namespace hn = hwy::HWY_NAMESPACE;

inline constexpr std::size_t kTargetLanes = hn::MaxLanes(hn::ScalableTag<double>());

template <std::size_t L, class F>
void CallWidth(const F& f) {
  if constexpr (L <= kTargetLanes) f(hn::FixedTag<double, L>());
}

template <class F>
void ForEachWidth(const F& f) {
  CallWidth<1>(f);
  CallWidth<2>(f);
  CallWidth<4>(f);
  CallWidth<8>(f);
}

inline bool SameBits(double a, double b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

}  // namespace aadp::simd::testing

#endif  // AADP_TESTS_SIMD_LANE_WIDTHS_HPP_
