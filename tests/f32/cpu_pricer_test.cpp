// The host side of the float32 lines (src/f32/cpu_pricer.hpp, paired.hpp): the reduction in
// double, the CPU line's independence of its thread count, what MakeParams accepts, and the paired
// statistics on numbers worked by hand.
#include "f32/cpu_pricer.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "f32/float_kernels.hpp"
#include "f32/paired.hpp"
#include "pricer/contract.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "rng/philox.hpp"

namespace aadp::f32 {
namespace {

constexpr pricer::Market<double> kMarket{
    .spot = 100.0, .sigma = 0.2, .r = 0.03, .q = 0.0, .maturity = 1.0};

pricer::Contract Asian(std::uint32_t fixings) {
  pricer::Contract c;
  c.product = pricer::Product::kAsianArithmetic;
  c.type = pricer::OptionType::kCall;
  c.strike = 100.0;
  c.steps = fixings;
  return c;
}

pricer::Contract OneStepUpOut() {
  pricer::Contract c;
  c.product = pricer::Product::kBarrier;
  c.type = pricer::OptionType::kCall;
  c.strike = 120.0;
  c.steps = 52;
  c.barrier = {.direction = pricer::Direction::kUp,
               .knock = pricer::Knock::kOut,
               .monitoring = pricer::Monitoring::kDiscrete,
               .treatment = pricer::Treatment::kOneStep,
               .level = 150.0,
               .smoothing_width = 0.0};
  return c;
}

bool SameBits(const std::vector<SpanSum>& a, const std::vector<SpanSum>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(SpanSum)) == 0;
}

// The pre-registered case: 2^25 span sums of 1. A float32 running sum stops at 2^24, where
// adding 1 rounds back; the host's double sum is exact.
TEST(F32Reduce, DoesNotStallWhereAFloatSumWould) {
  SpanSum one;
  one.sum = 1.0F;
  one.sum_squares = 1.0F;
  const std::vector<SpanSum> spans(std::size_t{1} << 25U, one);
  const Moments m = Reduce(spans, 1);
  EXPECT_EQ(m.sum, 0x1p25);
  EXPECT_EQ(m.sum_squares, 0x1p25);
  EXPECT_EQ(m.paths, std::uint64_t{1} << 25U);
}

TEST(F32Reduce, SumsInSpanOrderAndScalesToCurrency) {
  std::vector<SpanSum> spans(4);
  for (int i = 0; i < 4; ++i) {
    spans[static_cast<std::size_t>(i)].sum = static_cast<float>(i + 1);
    spans[static_cast<std::size_t>(i)].sum_squares = static_cast<float>((i + 1) * (i + 1));
  }
  const Moments m = Reduce(spans, 2);
  EXPECT_EQ(m.paths, 8U);
  const Estimate e = ToEstimate(m, 3.0);
  // 8 paths summing to 10, squares to 30: mean 1.25, variance (30 - 8 * 1.5625) / 7 = 2.5.
  EXPECT_DOUBLE_EQ(e.mean, 3.0 * 1.25);
  EXPECT_DOUBLE_EQ(e.standard_error, 3.0 * std::sqrt(2.5 / 8.0));
}

TEST(F32CpuLine, SameBitsOnAnyThreadCountAndSpanByHand) {
  for (const pricer::Contract& contract : {Asian(12), OneStepUpOut()}) {
    const auto params = MakeParams(contract, kMarket, 20261201, 16);
    ASSERT_TRUE(params.has_value());
    const std::uint64_t first = 0xFFFFFF00ULL;  // Crosses the 2^32 boundary of the path counter.
    const std::vector<SpanSum> one = PriceSpansCpu(*params, first, 40, 1);
    EXPECT_TRUE(SameBits(one, PriceSpansCpu(*params, first, 40, 3)));
    EXPECT_TRUE(SameBits(one, PriceSpansCpu(*params, first, 40, 8)));
    for (std::size_t s = 0; s < one.size(); ++s) {
      const std::uint64_t path = first + s * 16;
      const SpanSum direct =
          PriceSpan(*params, static_cast<Word>(path), static_cast<Word>(path >> 32U));
      ASSERT_EQ(std::bit_cast<std::uint64_t>(direct), std::bit_cast<std::uint64_t>(one[s])) << s;
    }
  }
}

TEST(F32Params, RoundsEachInputOnceAndRefusesWhatItCannotPrice) {
  const auto asian = MakeParams(Asian(12), kMarket, 0x0000000500000003ULL, 16);
  ASSERT_TRUE(asian.has_value());
  EXPECT_EQ(asian->product, static_cast<Word>(Product::kAsian));
  EXPECT_EQ(asian->seed_lo, 3U);
  EXPECT_EQ(asian->seed_hi, 5U);
  EXPECT_EQ(asian->drift, static_cast<float>((0.03 - 0.02) / 12.0));
  EXPECT_EQ(asian->sd, static_cast<float>(0.2 * std::sqrt(1.0 / 12.0)));
  EXPECT_EQ(asian->strike, 1.0F);
  const auto barrier = MakeParams(OneStepUpOut(), kMarket, 1, 16);
  ASSERT_TRUE(barrier.has_value());
  EXPECT_EQ(barrier->product, static_cast<Word>(Product::kUpOutOneStep));
  EXPECT_EQ(barrier->barrier, static_cast<float>(std::log(1.5)));
  EXPECT_DOUBLE_EQ(Scale(kMarket), 100.0 * std::exp(-0.03));

  EXPECT_EQ(MakeParams(Asian(12), kMarket, 1, 0).error(), SetupError::kSpan);
  EXPECT_EQ(MakeParams(Asian(12), kMarket, 1, kMaxSpan + 1).error(), SetupError::kSpan);
  pricer::Contract with_spot = Asian(12);
  with_spot.include_spot = true;
  EXPECT_EQ(MakeParams(with_spot, kMarket, 1, 16).error(), SetupError::kProduct);
  pricer::Contract smoothed = OneStepUpOut();
  smoothed.barrier.treatment = pricer::Treatment::kSmoothed;
  smoothed.barrier.smoothing_width = 0.05;
  EXPECT_EQ(MakeParams(smoothed, kMarket, 1, 16).error(), SetupError::kProduct);
  pricer::Contract bad = Asian(12);
  bad.strike = -1.0;
  EXPECT_EQ(MakeParams(bad, kMarket, 1, 16).error(), SetupError::kContract);
}

// Two columns over three spans of two paths, worked by hand: per-span differences 1, 2 and 6,
// mean 3 per span, 1.5 per path; sd of the span differences sqrt(7), SE sqrt(7) / (2 sqrt 3).
TEST(F32Paired, DifferenceAndItsStandardError) {
  Column a;
  a.span = 2;
  a.sum = {3.0, 5.0, 10.0};
  a.sum_squares = {5.0, 13.0, 50.0};
  Column b;
  b.span = 2;
  b.sum = {2.0, 3.0, 4.0};
  b.sum_squares = {2.0, 5.0, 8.0};
  const Difference d = PairedDifference(a, b);
  EXPECT_DOUBLE_EQ(d.mean, 1.5);
  EXPECT_DOUBLE_EQ(d.standard_error, std::sqrt(7.0) / (2.0 * std::sqrt(3.0)));
  const Estimate ea = ColumnEstimate(a);
  // 6 paths summing to 18, squares to 68: mean 3, variance (68 - 54) / 5.
  EXPECT_DOUBLE_EQ(ea.mean, 3.0);
  EXPECT_DOUBLE_EQ(ea.standard_error, std::sqrt(14.0 / 5.0 / 6.0));
}

// The double column is the production double kernel on the truncated uniforms: on one path it
// reproduces a hand-filled call of the same kernel.
TEST(F32DoubleColumn, IsTheProductionKernelOnTheTruncatedUniforms) {
  const pricer::Contract contract = Asian(12);
  const auto column = PriceSpansDouble(contract, kMarket, 20261201, 16, 1, UniformBits::k23, 2);
  const auto wider = PriceSpansDouble(contract, kMarket, 20261201, 16, 1, UniformBits::k32, 1);
  ASSERT_TRUE(column.has_value());
  ASSERT_TRUE(wider.has_value());
  // The first path in the money, by hand: Philox's first word, truncated to 23 bits.
  std::uint32_t path = 0;
  while (path < 16 && column->sum[path] == 0.0) ++path;
  ASSERT_LT(path, 16U);
  std::vector<double> normal(contract.steps);
  for (std::uint32_t step = 0; step < contract.steps; ++step) {
    const Philox4x32Output block = Philox4x32_10({path, 0U, step, 0U}, {20261201U, 0U});
    normal[step] = (static_cast<double>(block[0] >> 9U) + 0.5) * 0x1p-23;
  }
  const pricer::GbmModel<double> model(kMarket, contract.steps);
  EXPECT_EQ(column->sum[path],
            pricer::DiscountedPayoff<double>(contract, model, {.normal = normal, .crossing = {}}));
  EXPECT_EQ(column->sum_squares[path], column->sum[path] * column->sum[path]);
  EXPECT_NE(column->sum[path], wider->sum[path]);
  EXPECT_EQ(PriceSpansDouble(contract, kMarket, 1, 3, 0, UniformBits::k23, 1).error(),
            SetupError::kSpan);
}

}  // namespace
}  // namespace aadp::f32
