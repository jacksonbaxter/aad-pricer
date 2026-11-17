// xoshiro256++ (rng/xoshiro256pp.hpp) against published known-answer values: the first ten
// outputs from the state (1, 2, 3, 4), as the rand_xoshiro crate's reference test gives them
// (rust-random/rngs, rand_xoshiro/src/xoshiro256plusplus.rs), which says they were produced with
// the authors' reference implementation. Only the numbers are taken from it.
#include "rng/xoshiro256pp.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace aadp {
namespace {

constexpr Xoshiro256PlusPlus::State kState = {1, 2, 3, 4};

constexpr std::array<std::uint64_t, 10> kExpected = {
    41943041ULL,
    58720359ULL,
    3588806011781223ULL,
    3591011842654386ULL,
    9228616714210784205ULL,
    9973669472204895162ULL,
    14011001112246962877ULL,
    12406186145184390807ULL,
    15849039046786891736ULL,
    10450023813501588000ULL,
};

// By hand: rotl(1 + 4, 23) + 1 = 5 * 2^23 + 1.
static_assert(Xoshiro256PlusPlus(kState).Next() == 41943041ULL);

TEST(Xoshiro256PlusPlus, FirstTenOutputsAreThePublishedOnes) {
  Xoshiro256PlusPlus generator(kState);
  for (std::size_t i = 0; i < kExpected.size(); ++i) {
    EXPECT_EQ(generator.Next(), kExpected[i]) << "output " << i;
  }
}

// The engine's first step by hand: t = 2 << 17; s2 = 3 ^ 1 = 2, s3 = 4 ^ 2 = 6, s1 = 2 ^ 2 = 0,
// s0 = 1 ^ 6 = 7, s2 = 2 ^ t, s3 = rotl(6, 45).
TEST(Xoshiro256PlusPlus, OneStepOfTheEngine) {
  Xoshiro256PlusPlus generator(kState);
  generator.Next();
  const Xoshiro256PlusPlus::State expected = {7, 0, 2 ^ (std::uint64_t{2} << 17),
                                              std::uint64_t{6} << 45};
  EXPECT_EQ(generator.state(), expected);
}

}  // namespace
}  // namespace aadp
