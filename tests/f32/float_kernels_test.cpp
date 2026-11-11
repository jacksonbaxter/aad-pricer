// The shared float32 kernels on the CPU (src/f32/float_kernels.hpp), against the double code they
// copy. Every build and host; tests/metal/ runs the same source on the GPU.
//
// Tolerances, stated before measuring, each with its reason:
// - InverseNormalF against AS241 in double, on all 2^23 uniforms of the grid: within 16 ULP of
//   max(|z|, 1) in float. Giles gives about 4 ULP in the centre and about 6 in the tail for his
//   single-precision erfinv (metal-gpu/EVIDENCE.md, E47); writing w from 4 q (1 - q) and the final
//   sqrt(2) product add about two roundings, and the host's logf is not CUDA's. A factor of two on
//   that leaves room; a mistyped coefficient is off by far more.
// - NormalCdfF against pricer::NormalCdf in double: |p_f - p_d| <= (16 + 4 y^2) 2^-24 p_d, y =
//   -x / sqrt 2, wherever p_d is a normal float. Cody's fit is far below float precision; the
//   float version adds two exps (libm's, within an ULP each), about a dozen roundings in the
//   rationals and the products, so 16 ULP; and the roundings of y and of sqrt 2, each a relative
//   2^-24 of y, move exp(-y^2) by a relative 2 y^2 2^-24 each, so 4 y^2 more.
#include "f32/float_kernels.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>

#include "f32/paired.hpp"
#include "math/inverse_normal.hpp"
#include "pricer/normal_cdf.hpp"
#include "rng/philox.hpp"

namespace aadp::f32 {
namespace {

// The ULP of a float near |x|: the spacing of floats at max(|x|, the smallest normal).
double FloatUlp(double x) {
  const auto magnitude = static_cast<float>(std::fabs(x));
  const float at =
      magnitude < std::numeric_limits<float>::min() ? std::numeric_limits<float>::min() : magnitude;
  return static_cast<double>(std::nextafter(at, std::numeric_limits<float>::infinity()) - at);
}

struct KnownAnswer {
  Philox4x32Counter counter;
  Philox4x32Key key;
  Philox4x32Output ten;
  Philox4x32Output seven;  // zero where Random123's file has no 7-round line
};

// The vectors of tests/oracle/philox_test.cpp, which cites Random123's file and its hash.
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

Philox4x32Output Shared(int rounds, const Philox4x32Counter& c, const Philox4x32Key& k) {
  Words4 counter;
  counter.w0 = c[0];
  counter.w1 = c[1];
  counter.w2 = c[2];
  counter.w3 = c[3];
  const Words4 r = PhiloxRounds(rounds, counter, k[0], k[1]);
  return {r.w0, r.w1, r.w2, r.w3};
}

TEST(F32Philox, KnownAnswersTenAndSevenRounds) {
  for (const KnownAnswer& v : kVectors) {
    EXPECT_EQ(Shared(10, v.counter, v.key), v.ten);
    if (v.seven != Philox4x32Output{}) {
      EXPECT_EQ(Shared(7, v.counter, v.key), v.seven);
    }
  }
}

// DrawBlock is the block the production pricer draws for (seed; path, step), across the 2^32
// boundary of the path counter and with seeds that use their high word.
TEST(F32Philox, DrawBlockIsTheProductionBlock) {
  constexpr std::array<std::uint64_t, 3> kSeeds = {20261201, 0x0000000100000007ULL,
                                                   0xFFFFFFFFFFFFFFFFULL};
  constexpr std::array<std::uint64_t, 4> kPaths = {0, 12345, 0xFFFFFFFFULL, 0x100000000ULL};
  int compared = 0;
  for (const std::uint64_t seed : kSeeds) {
    for (const std::uint64_t path : kPaths) {
      for (std::uint32_t step = 0; step < 300; step += 7) {
        const Philox4x32Output expected = Philox4x32_10(
            {static_cast<std::uint32_t>(path), static_cast<std::uint32_t>(path >> 32U), step, 0U},
            {static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32U)});
        const Words4 got = DrawBlock(static_cast<Word>(seed), static_cast<Word>(seed >> 32U),
                                     static_cast<Word>(path), static_cast<Word>(path >> 32U), step);
        ASSERT_EQ((Philox4x32Output{got.w0, got.w1, got.w2, got.w3}), expected)
            << "seed " << seed << " path " << path << " step " << step;
        ++compared;
      }
    }
  }
  EXPECT_EQ(compared, 3 * 4 * 43);
}

// Every one of the 2^23 cells, with low bits that the uniform must ignore.
TEST(F32Uniform, ExactOnTheGridNeverZeroOrOne) {
  for (std::uint32_t cell = 0; cell < (1U << 23U); ++cell) {
    const std::uint32_t word = (cell << 9U) | ((cell * 2654435761U) & 0x1FFU);
    const float u = Uniform23(word);
    ASSERT_EQ(static_cast<double>(u), UniformFromWord(word, UniformBits::k23)) << word;
  }
  EXPECT_EQ(Uniform23(0U), 0x1p-24F);
  EXPECT_EQ(Uniform23(0xFFFFFFFFU), 1.0F - 0x1p-24F);
  EXPECT_LT(Uniform23(0xFFFFFFFFU), 1.0F);
  // The 32-bit uniform of the double column 32 bits: the top word's own value.
  EXPECT_EQ(UniformFromWord(0U, UniformBits::k32), 0x1p-33);
  EXPECT_EQ(UniformFromWord(0xFFFFFFFFU, UniformBits::k32), 1.0 - 0x1p-33);
}

// Symmetric to the bit, and within the stated bound of AS241, on every grid point.
TEST(F32InverseNormal, SymmetricAndWithinBoundOnTheWholeGrid) {
  double worst = 0.0;
  double worst_u = 0.0;
  for (std::uint32_t cell = 0; cell < (1U << 23U); ++cell) {
    const float u = Uniform23(cell << 9U);
    const float z = InverseNormalF(u);
    const float mirror = InverseNormalF(1.0F - u);
    ASSERT_EQ(std::bit_cast<std::uint32_t>(mirror), std::bit_cast<std::uint32_t>(-z)) << u;
    const double exact = math::inverse_normal(static_cast<double>(u));
    const double ulps =
        std::fabs(static_cast<double>(z) - exact) / FloatUlp(std::fmax(std::fabs(exact), 1.0));
    if (ulps > worst) {
      worst = ulps;
      worst_u = u;
    }
  }
  std::printf("InverseNormalF: worst %.3f ULP of max(|z|, 1) at u = %.9g\n", worst, worst_u);
  EXPECT_LE(worst, 16.0);
}

TEST(F32NormalCdf, WithinBoundWherePhiIsANormalFloat) {
  double worst = 0.0;
  double worst_x = 0.0;
  int compared = 0;
  for (int i = -13 * 1024; i <= 9 * 1024; ++i) {
    const auto x = static_cast<float>(i) / 1024.0F;
    const double exact = pricer::NormalCdf(static_cast<double>(x));
    if (exact < static_cast<double>(std::numeric_limits<float>::min())) continue;
    const double y = -static_cast<double>(x) / std::numbers::sqrt2;
    const double allowed = (16.0 + 4.0 * y * y) * 0x1p-24 * exact;
    const double error = std::fabs(static_cast<double>(NormalCdfF(x)) - exact);
    if (error / allowed > worst) {
      worst = error / allowed;
      worst_x = x;
    }
    ++compared;
  }
  std::printf("NormalCdfF: %d points, worst %.3f of the bound at x = %.6g\n", compared, worst,
              worst_x);
  EXPECT_LE(worst, 1.0);
  EXPECT_GT(compared, 20000);
}

// The case the pre-registration names: 1 and then 2^16 terms of 0.75 2^-24. Each term is below
// half an ULP of 1, so a plain float sum never moves; the compensated sum carries them.
TEST(F32Compensated, CarriesWhatAPlainSumDrops) {
  Compensated compensated;
  compensated = CompensatedAdd(compensated, 1.0F);
  float plain = 1.0F;
  for (int i = 0; i < (1 << 16); ++i) {
    compensated = CompensatedAdd(compensated, 0.75F * 0x1p-24F);
    plain += 0.75F * 0x1p-24F;
  }
  EXPECT_EQ(plain, 1.0F);
  EXPECT_EQ(compensated.sum, 1.0F + 0.75F * 0x1p-8F);
}

}  // namespace
}  // namespace aadp::f32
