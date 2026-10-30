// Philox4x32-10 (Salmon et al., SC11), scalar. Keyed by seed, counter (path, step), so a
// draw depends only on those three numbers and never on lanes, threads or call order.
//
// Known-answer vectors: tests/oracle/philox_test.cpp, from Random123 tests/kat_vectors.
#ifndef AADP_RNG_PHILOX_HPP_
#define AADP_RNG_PHILOX_HPP_

#include <array>
#include <cstdint>

namespace aadp {

using Philox4x32Counter = std::array<std::uint32_t, 4>;
using Philox4x32Key = std::array<std::uint32_t, 2>;
using Philox4x32Output = std::array<std::uint32_t, 4>;

namespace philox_detail {

inline constexpr std::uint32_t kMultiplier0 = 0xD2511F53U;
inline constexpr std::uint32_t kMultiplier1 = 0xCD9E8D57U;
inline constexpr std::uint32_t kWeyl0 = 0x9E3779B9U;
inline constexpr std::uint32_t kWeyl1 = 0xBB67AE85U;

constexpr std::uint32_t High32(std::uint64_t x) { return static_cast<std::uint32_t>(x >> 32); }
constexpr std::uint32_t Low32(std::uint64_t x) { return static_cast<std::uint32_t>(x); }

}  // namespace philox_detail

constexpr Philox4x32Output Philox4x32R(int rounds, Philox4x32Counter counter, Philox4x32Key key) {
  using namespace philox_detail;
  for (int round = 0; round < rounds; ++round) {
    if (round > 0) {
      key[0] += kWeyl0;
      key[1] += kWeyl1;
    }
    const std::uint64_t product0 = std::uint64_t{kMultiplier0} * counter[0];
    const std::uint64_t product1 = std::uint64_t{kMultiplier1} * counter[2];
    counter = {High32(product1) ^ counter[1] ^ key[0], Low32(product1),
               High32(product0) ^ counter[3] ^ key[1], Low32(product0)};
  }
  return counter;
}

constexpr Philox4x32Output Philox4x32_10(Philox4x32Counter counter, Philox4x32Key key) {
  return Philox4x32R(10, counter, key);
}

// Odd multiples of 2^-53, so 0 and 1 are unreachable and the inverse normal stays finite.
// A 52-bit grid, because (k + 0.5) * 2^-53 on 53 bits rounds up to exactly 1.0 at the top.
constexpr double UniformFromBits(std::uint64_t bits) {
  constexpr double kGrid = 1.0 / 4503599627370496.0;  // 2^-52
  return (static_cast<double>(bits >> 12) + 0.5) * kGrid;
}

// The seed is the key. Path takes two counter words and step a third. The fourth is zero and
// free for a later domain tag.
constexpr double PathUniform(std::uint64_t seed, std::uint64_t path, std::uint32_t step) {
  const Philox4x32Output out = Philox4x32_10(
      {static_cast<std::uint32_t>(path), static_cast<std::uint32_t>(path >> 32), step, 0U},
      {static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32)});
  return UniformFromBits((std::uint64_t{out[0]} << 32) | out[1]);
}

}  // namespace aadp

#endif  // AADP_RNG_PHILOX_HPP_
