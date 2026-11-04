// Check 2 of stage 5a: Philox4x32-10 on lanes. Known answers from Random123 (the vectors of
// tests/oracle/philox_test.cpp, which cites the file and its hash), one vector per lane with its
// own key, at every width; and lane i of a group drawing exactly what scalar path first + i draws,
// for both uniforms of a step, across the 2^32 boundary of the path counter.
#include "simd/philox_lanes.hpp"

#include <gtest/gtest.h>
#include <hwy/highway.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

#include "lane_widths.hpp"
#include "pricer/draws.hpp"
#include "rng/philox.hpp"

namespace aadp::simd {
namespace {

namespace hn = hwy::HWY_NAMESPACE;
using testing::ForEachWidth;
using testing::SameBits;

struct KnownAnswer {
  Philox4x32Counter counter;
  Philox4x32Key key;
  Philox4x32Output ten;
  Philox4x32Output seven;  // zero where the file has no 7-round line for the vector
};

constexpr auto kVectors = std::to_array<KnownAnswer>({
    {.counter = {0x00000000, 0x00000000, 0x00000000, 0x00000000},
     .key = {0x00000000, 0x00000000},
     .ten = {0x6627e8d5, 0xe169c58d, 0xbc57ac4c, 0x9b00dbd8},
     .seven = {0x5f6fb709, 0x0d893f64, 0x4f121f81, 0x4f730a48}},
    {.counter = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
     .key = {0xffffffff, 0xffffffff},
     .ten = {0x408f276d, 0x41c83b0e, 0xa20bc7c6, 0x6d5451fd},
     .seven = {}},
    {.counter = {0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344},
     .key = {0xa4093822, 0x299f31d0},
     .ten = {0xd16cfe09, 0x94fdcceb, 0x5001e420, 0x24126ea1},
     .seven = {0x4dfccaba, 0x190a87f0, 0xc47362ba, 0xb6b5242a}},
});

// Lane i carries vector (i + shift) mod 3, so each vector meets each lane.
template <class D>
std::array<std::array<std::uint32_t, 4>, 8> RunLanes(D /*d*/, int rounds, std::size_t shift) {
  using DU = hn::RebindToUnsigned<D>;
  const DU du;
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  std::array<std::array<std::uint64_t, kLanes>, 6> in{};
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    const KnownAnswer& v = kVectors[(lane + shift) % kVectors.size()];
    for (std::size_t w = 0; w < 4; ++w) in[w][lane] = v.counter[w];
    in[4][lane] = v.key[0];
    in[5][lane] = v.key[1];
  }
  const PhiloxWords<DU> counter = {hn::LoadU(du, in[0].data()), hn::LoadU(du, in[1].data()),
                                   hn::LoadU(du, in[2].data()), hn::LoadU(du, in[3].data())};
  const PhiloxKeyWords<DU> key = {hn::LoadU(du, in[4].data()), hn::LoadU(du, in[5].data())};
  const PhiloxWords<DU> out = Philox4x32RLanes(du, rounds, counter, key);
  std::array<std::array<std::uint32_t, 4>, 8> result{};
  for (std::size_t w = 0; w < 4; ++w) {
    std::array<std::uint64_t, kLanes> words{};
    hn::StoreU(out[w], du, words.data());
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      // Every word must stay inside its 32 bits, or the next round would read garbage.
      EXPECT_EQ(words[lane] >> 32, 0U) << "word " << w << " lane " << lane;
      result[lane][w] = static_cast<std::uint32_t>(words[lane]);
    }
  }
  return result;
}

TEST(SimdPhilox, KnownAnswersOnEveryLane) {
  ForEachWidth([](auto d) {
    constexpr std::size_t kLanes = hn::MaxLanes(decltype(d)());
    std::size_t checked = 0;
    for (std::size_t shift = 0; shift < kVectors.size(); ++shift) {
      const auto ten = RunLanes(d, 10, shift);
      const auto seven = RunLanes(d, 7, shift);
      for (std::size_t lane = 0; lane < kLanes; ++lane) {
        const KnownAnswer& v = kVectors[(lane + shift) % kVectors.size()];
        EXPECT_EQ(ten[lane], v.ten) << kLanes << " lanes, lane " << lane << ", 10 rounds";
        ++checked;
        if (v.seven != Philox4x32Output{}) {
          EXPECT_EQ(seven[lane], v.seven) << kLanes << " lanes, lane " << lane << ", 7 rounds";
          ++checked;
        }
      }
    }
    std::cout << "simd philox: " << kLanes << " lanes, " << checked << " known answers\n";
  });
}

TEST(SimdPhilox, LaneIDrawsWhatScalarPathFirstPlusIDraws) {
  // Development seeds, never 1729 or 271828; the last reaches the key's high word.
  std::vector<std::uint64_t> seeds = {20261115, 0, 0xffffffffffffffffU, 0x00000001deadbeefU};
  // The last first paths straddle the high word of the counter.
  std::vector<std::uint64_t> firsts = {
      0, 1, 1023, 4096 + 5, 0xfffffffcU, 0xffffffffU, 0x00000007fffffffdU};
  std::vector<std::uint32_t> steps = {0U, 1U, 251U, 0xffffffffU};
  // And arbitrary coordinates, every word of them live.
  std::mt19937_64 rng(20261118);
  for (int i = 0; i < 6; ++i) {
    seeds.push_back(rng());
    firsts.push_back(rng() >> 1);
    steps.push_back(static_cast<std::uint32_t>(rng()));
  }
  ForEachWidth([&](auto d) {
    using D = decltype(d);
    constexpr std::size_t kLanes = hn::MaxLanes(D());
    std::size_t checked = 0;
    std::size_t mismatches = 0;
    std::array<double, kLanes> normal{};
    std::array<double, kLanes> crossing{};
    for (const std::uint64_t seed : seeds) {
      for (const std::uint64_t first : firsts) {
        for (const std::uint32_t step : steps) {
          const StepUniformLanes<D> u = DrawStepLanes(d, seed, first, step);
          hn::StoreU(u.normal, d, normal.data());
          hn::StoreU(u.crossing, d, crossing.data());
          for (std::size_t lane = 0; lane < kLanes; ++lane) {
            const pricer::StepUniforms s = pricer::DrawStep(seed, first + lane, step);
            ++checked;
            if (!SameBits(normal[lane], s.normal) || !SameBits(crossing[lane], s.crossing)) {
              ++mismatches;
              ADD_FAILURE() << kLanes << " lanes: seed " << seed << ", path " << first + lane
                            << ", step " << step << ": lane " << normal[lane] << ", "
                            << crossing[lane] << "; scalar " << s.normal << ", " << s.crossing;
            }
          }
        }
      }
    }
    std::cout << "simd philox draws: " << kLanes << " lanes, " << checked << " draws, "
              << mismatches << " not bit-identical\n";
  });
}

}  // namespace
}  // namespace aadp::simd
