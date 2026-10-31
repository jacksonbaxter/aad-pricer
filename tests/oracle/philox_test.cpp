// Known answers from Random123, tests/kat_vectors, lines "philox4x32 10 ...", at
// https://raw.githubusercontent.com/DEShawResearch/random123/
// d8a0c25e49992685c0b9d91b553c1d04f81b6b4b/tests/kat_vectors
// File sha256 aab5ebabf40003f63d6d87b24cbd2c8a02652e00cf8bad64226fd50586929183.
#include "rng/philox.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>

namespace aadp {
namespace {

struct KnownAnswer {
  Philox4x32Counter counter;
  Philox4x32Key key;
  Philox4x32Output expected;
};

constexpr auto kVectors = std::to_array<KnownAnswer>({
    {.counter = {0x00000000, 0x00000000, 0x00000000, 0x00000000},
     .key = {0x00000000, 0x00000000},
     .expected = {0x6627e8d5, 0xe169c58d, 0xbc57ac4c, 0x9b00dbd8}},
    {.counter = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
     .key = {0xffffffff, 0xffffffff},
     .expected = {0x408f276d, 0x41c83b0e, 0xa20bc7c6, 0x6d5451fd}},
    {.counter = {0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344},
     .key = {0xa4093822, 0x299f31d0},
     .expected = {0xd16cfe09, 0x94fdcceb, 0x5001e420, 0x24126ea1}},
});

TEST(Philox, KnownAnswersTenRounds) {
  for (const KnownAnswer& v : kVectors) {
    EXPECT_EQ(Philox4x32_10(v.counter, v.key), v.expected);
  }
}

TEST(Philox, KnownAnswersAreCheckedAtCompileTime) {
  static_assert(Philox4x32_10(kVectors[2].counter, kVectors[2].key) == kVectors[2].expected);
}

// The same file carries 7-round vectors. They reach the key schedule at a different depth, so
// a wrong round count cannot match by accident.
TEST(Philox, KnownAnswersSevenRounds) {
  EXPECT_EQ(Philox4x32R(7, {0, 0, 0, 0}, {0, 0}),
            (Philox4x32Output{0x5f6fb709, 0x0d893f64, 0x4f121f81, 0x4f730a48}));
  EXPECT_EQ(
      Philox4x32R(7, {0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344}, {0xa4093822, 0x299f31d0}),
      (Philox4x32Output{0x4dfccaba, 0x190a87f0, 0xc47362ba, 0xb6b5242a}));
}

TEST(PathUniform, StaysInsideTheOpenUnitInterval) {
  EXPECT_EQ(UniformFromBits(0), 0x1p-53);
  EXPECT_EQ(UniformFromBits(~std::uint64_t{0}), 1.0 - 0x1p-53);
  EXPECT_LT(UniformFromBits(~std::uint64_t{0}), 1.0);
  EXPECT_GT(UniformFromBits(0), 0.0);
}

TEST(PathUniform, EveryCoordinateChangesTheDraw) {
  const double base = PathUniform(7, 11, 3);
  EXPECT_NE(base, PathUniform(8, 11, 3));
  EXPECT_NE(base, PathUniform(7, 12, 3));
  EXPECT_NE(base, PathUniform(7, 11, 4));
  // High words reach the counter and key.
  EXPECT_NE(base, PathUniform(7 + (std::uint64_t{1} << 32), 11, 3));
  EXPECT_NE(base, PathUniform(7, 11 + (std::uint64_t{1} << 32), 3));
  EXPECT_EQ(base, PathUniform(7, 11, 3));
}

// A blunt sanity bound, not a statistical test of the generator. For 2^16 draws the mean has SE
// sqrt(1/12 / 2^16) = 1.13e-3 and the variance estimate SE sqrt((1/80 - 1/144) / 2^16) = 2.9e-4,
// so a working stream stays inside 6 SE on this fixed seed and a stuck bit does not.
TEST(PathUniform, FirstTwoMomentsMatchUniform) {
  constexpr int kDraws = 1 << 16;
  double sum = 0.0;
  double sum_squares = 0.0;
  std::set<double> distinct;
  for (int i = 0; i < kDraws; ++i) {
    const double u =
        PathUniform(20261030, static_cast<std::uint64_t>(i) / 7, static_cast<std::uint32_t>(i % 7));
    sum += u;
    sum_squares += (u - 0.5) * (u - 0.5);
    distinct.insert(u);
  }
  EXPECT_EQ(distinct.size(), static_cast<std::size_t>(kDraws));
  EXPECT_NEAR(sum / kDraws, 0.5, 6.0 * 1.13e-3);
  EXPECT_NEAR(sum_squares / kDraws, 1.0 / 12.0, 6.0 * 2.9e-4);
}

}  // namespace
}  // namespace aadp
