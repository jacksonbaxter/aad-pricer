// simd::PhiloxArray, RNG-1's throughput kernel: every word is the scalar generator's
// (rng/philox.hpp, itself checked against Random123's known answers in tests/oracle/), at one lane
// and at the ceiling, over counters that cross into the high word, with tails shorter than a
// vector and a length that is not a whole number of blocks.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rng/philox.hpp"
#include "simd/simd_pricer.hpp"

namespace aadp::simd {
namespace {

constexpr std::uint32_t kUntouched = 0xa5a5a5a5U;

std::vector<std::uint32_t> Expected(std::uint64_t seed, std::uint64_t first, std::size_t size) {
  std::vector<std::uint32_t> out(size, kUntouched);
  for (std::size_t b = 0; b < size / 4; ++b) {
    const std::uint64_t c = first + b;
    const Philox4x32Output words =
        Philox4x32_10({static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(c >> 32), 0U, 0U},
                      {static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32)});
    for (std::size_t i = 0; i < 4; ++i) out[(4 * b) + i] = words[i];
  }
  return out;
}

std::vector<std::size_t> Widths() {
  std::vector<std::size_t> widths{1};
  if (MaxLanes() != 1) widths.push_back(MaxLanes());
  return widths;
}

TEST(PhiloxArray, BlockZeroUnderKeyZeroIsRandom123sKnownAnswer) {
  for (const std::size_t lanes : Widths()) {
    std::vector<std::uint32_t> out(4, kUntouched);
    ASSERT_TRUE(PhiloxArray(0, 0, out, lanes));
    EXPECT_EQ(out, (std::vector<std::uint32_t>{0x6627e8d5U, 0xe169c58dU, 0xbc57ac4cU, 0x9b00dbd8U}))
        << lanes << " lanes";
  }
}

TEST(PhiloxArray, EveryWordIsTheScalarGenerators) {
  const std::uint64_t seed = 0x0123456789abcdefULL;  // A development seed, not a reserved one
  for (const std::size_t lanes : Widths()) {
    for (const std::uint64_t first :
         {std::uint64_t{0}, std::uint64_t{0xfffffffdU}, (std::uint64_t{1} << 40) + 7}) {
      for (const std::size_t blocks : {std::size_t{0}, std::size_t{1}, std::size_t{3},
                                       std::size_t{8}, std::size_t{37}, std::size_t{1000}}) {
        for (const std::size_t extra : {std::size_t{0}, std::size_t{3}}) {
          const std::size_t size = (4 * blocks) + extra;
          std::vector<std::uint32_t> out(size, kUntouched);
          ASSERT_TRUE(PhiloxArray(seed, first, out, lanes));
          EXPECT_EQ(out, Expected(seed, first, size))
              << lanes << " lanes, first " << first << ", " << blocks << " blocks, " << extra
              << " words over";
        }
      }
    }
  }
}

TEST(PhiloxArray, AnyOtherLaneCountWritesNothing) {
  for (const std::size_t lanes : {std::size_t{0}, std::size_t{3}, 2 * MaxLanes()}) {
    if (lanes == 1 || lanes == MaxLanes()) continue;
    std::vector<std::uint32_t> out(16, kUntouched);
    EXPECT_FALSE(PhiloxArray(1, 0, out, lanes)) << lanes;
    EXPECT_EQ(out, std::vector<std::uint32_t>(16, kUntouched)) << lanes;
  }
}

}  // namespace
}  // namespace aadp::simd
