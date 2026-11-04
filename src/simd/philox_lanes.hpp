// Philox4x32-10 on Highway lanes, and the pricer's per-step uniforms from it.
//
// Lane i of a block of paths draws exactly what scalar path i draws: the counter is
// (path, step) as in src/pricer/draws.hpp, with path = first + i, and the key is the seed. Each
// 32-bit word sits in the low half of a 64-bit lane, one lane per path, so lane i of the words is
// lane i of the doubles they become. The 32 x 32 -> 64 products are hn::MulEven on the words seen
// as 32-bit lanes (the low halves, on these little-endian targets).
//
// Known answers: tests/simd/philox_lanes_test.cpp, the Random123 vectors of
// tests/oracle/philox_test.cpp, one per lane.
#ifndef AADP_SIMD_PHILOX_LANES_HPP_
#define AADP_SIMD_PHILOX_LANES_HPP_

#include <hwy/highway.h>

#include <array>
#include <bit>
#include <cstdint>

#include "rng/philox.hpp"

HWY_BEFORE_NAMESPACE();

namespace aadp::simd {

namespace hn = hwy::HWY_NAMESPACE;

// Four 32-bit words per lane, each in the low half of a 64-bit lane of the unsigned tag DU.
template <class DU>
using PhiloxWords = std::array<hn::Vec<DU>, 4>;
template <class DU>
using PhiloxKeyWords = std::array<hn::Vec<DU>, 2>;

// Philox4x32R of rng/philox.hpp, lane by lane.
template <class DU>
PhiloxWords<DU> Philox4x32RLanes(DU du, int rounds, PhiloxWords<DU> counter,
                                 PhiloxKeyWords<DU> key) {
  using namespace philox_detail;
  const hn::Repartition<std::uint32_t, DU> d32;
  const auto low = hn::Set(du, std::uint64_t{0xffffffffU});
  const auto multiplier0 = hn::BitCast(d32, hn::Set(du, std::uint64_t{kMultiplier0}));
  const auto multiplier1 = hn::BitCast(d32, hn::Set(du, std::uint64_t{kMultiplier1}));
  const auto weyl0 = hn::Set(du, std::uint64_t{kWeyl0});
  const auto weyl1 = hn::Set(du, std::uint64_t{kWeyl1});
  for (int round = 0; round < rounds; ++round) {
    if (round > 0) {
      key[0] = hn::And(hn::Add(key[0], weyl0), low);
      key[1] = hn::And(hn::Add(key[1], weyl1), low);
    }
    const auto product0 = hn::MulEven(multiplier0, hn::BitCast(d32, counter[0]));
    const auto product1 = hn::MulEven(multiplier1, hn::BitCast(d32, counter[2]));
    counter = {
        hn::Xor(hn::Xor(hn::ShiftRight<32>(product1), counter[1]), key[0]), hn::And(product1, low),
        hn::Xor(hn::Xor(hn::ShiftRight<32>(product0), counter[3]), key[1]), hn::And(product0, low)};
  }
  return counter;
}

template <class DU>
PhiloxWords<DU> Philox4x32_10Lanes(DU du, PhiloxWords<DU> counter, PhiloxKeyWords<DU> key) {
  return Philox4x32RLanes(du, 10, counter, key);
}

// UniformFromBits of rng/philox.hpp, lane by lane. bits >> 12 is below 2^52, so adding it to the
// bits of 2^52 and subtracting 2^52 is its conversion to double, exactly.
template <class D>
hn::Vec<D> UniformFromBitsLanes(D d, hn::Vec<hn::RebindToUnsigned<D>> bits) {
  const hn::RebindToUnsigned<D> du;
  constexpr double kTwo52 = 0x1p52;
  constexpr double kGrid = 1.0 / 4503599627370496.0;  // 2^-52, as there
  const auto mantissa = hn::ShiftRight<12>(bits);
  const auto power = hn::Set(du, std::bit_cast<std::uint64_t>(kTwo52));
  const hn::Vec<D> k = hn::Sub(hn::BitCast(d, hn::Or(mantissa, power)), hn::Set(d, kTwo52));
  return hn::Mul(hn::Add(k, hn::Set(d, 0.5)), hn::Set(d, kGrid));
}

template <class D>
struct StepUniformLanes {
  hn::Vec<D> normal;
  hn::Vec<D> crossing;
};

// pricer::DrawStep for paths first, first + 1, ..., one per lane.
template <class D>
StepUniformLanes<D> DrawStepLanes(D d, std::uint64_t seed, std::uint64_t first,
                                  std::uint32_t step) {
  const hn::RebindToUnsigned<D> du;
  const auto low = hn::Set(du, std::uint64_t{0xffffffffU});
  const auto path = hn::Iota(du, first);
  const PhiloxWords<hn::RebindToUnsigned<D>> counter = {
      hn::And(path, low), hn::ShiftRight<32>(path), hn::Set(du, std::uint64_t{step}), hn::Zero(du)};
  const PhiloxKeyWords<hn::RebindToUnsigned<D>> key = {hn::Set(du, seed & 0xffffffffU),
                                                       hn::Set(du, seed >> 32)};
  const auto out = Philox4x32_10Lanes(du, counter, key);
  return {.normal = UniformFromBitsLanes(d, hn::Or(hn::ShiftLeft<32>(out[0]), out[1])),
          .crossing = UniformFromBitsLanes(d, hn::Or(hn::ShiftLeft<32>(out[2]), out[3]))};
}

}  // namespace aadp::simd

HWY_AFTER_NAMESPACE();

#endif  // AADP_SIMD_PHILOX_LANES_HPP_
