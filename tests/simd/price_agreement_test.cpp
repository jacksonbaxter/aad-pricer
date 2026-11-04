// Check 3 of stage 5a: PriceLanes gives Price()'s estimate, bit for bit (mean, SE and every batch
// mean), on every product and treatment, at every lane width this build can run, on 1, 2, 3 and 8
// threads. Each batch is three blocks and a part block of 17 paths, so a lane group runs past a
// block's end at every width above one, and workers take blocks out of order.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

#include "build_info.hpp"
#include "cases.hpp"
#include "lane_widths.hpp"
#include "pricer/contract.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "simd/simd_pricer.hpp"

#ifndef AADP_SMOKE_SIZE
#define AADP_SMOKE_SIZE 0
#endif

namespace aadp::simd {
namespace {

using pricer::Market;
using testing::SameBits;

constexpr Market<double> kMarket{
    .spot = 100.0, .sigma = 0.3, .r = 0.05, .q = 0.02, .maturity = 1.0};
// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261117;
constexpr std::uint32_t kBatches = AADP_SMOKE_SIZE ? 2 : 4;
constexpr std::uint64_t kPaths = kBatches * ((AADP_SMOKE_SIZE ? 1 : 3) * pricer::kBlockPaths + 17);

bool SameEstimate(const pricer::Estimate& a, const pricer::Estimate& b) {
  if (!SameBits(a.mean, b.mean) || !SameBits(a.standard_error, b.standard_error)) return false;
  if (a.batch_means.size() != b.batch_means.size()) return false;
  for (std::size_t i = 0; i < a.batch_means.size(); ++i) {
    if (!SameBits(a.batch_means[i], b.batch_means[i])) return false;
  }
  return true;
}

TEST(SimdPrices, EveryProductIsBitIdenticalForEveryWidthAndThreadCount) {
  std::size_t pricings = 0;
  const auto cases = testing::AllCases(12);
  for (const testing::Case& c : cases) {
    const pricer::RunConfig base{.seed = kSeed, .paths = kPaths, .batches = kBatches};
    const auto scalar = pricer::Price(
        c.contract, kMarket, {.seed = kSeed, .paths = kPaths, .batches = kBatches, .threads = 1});
    ASSERT_TRUE(scalar.has_value()) << c.name;
    for (const std::size_t lanes : LaneWidths()) {
      for (const std::uint32_t threads : {1U, 2U, 3U, 8U}) {
        pricer::RunConfig config = base;
        config.threads = threads;
        const auto simd = PriceLanes(c.contract, kMarket, config, lanes);
        ASSERT_TRUE(simd.has_value()) << c.name;
        ++pricings;
        EXPECT_TRUE(SameEstimate(*simd, *scalar))
            << c.name << ", " << lanes << " lanes, " << threads << " threads: " << simd->mean
            << " against " << scalar->mean;
      }
    }
  }
  std::cout << "simd prices: " << cases.size() << " cases, widths up to " << MaxLanes() << ", "
            << pricings << " pricings of " << kPaths << " paths compared with Price()\n";
}

TEST(SimdPrices, RefusesWhatPriceRefusesAndUnknownWidths) {
  const auto cases = testing::AllCases(12);
  const pricer::Contract& c = cases.front().contract;
  const pricer::RunConfig ok{.seed = kSeed, .paths = 64, .batches = 2, .threads = 1};
  Market<double> bad = kMarket;
  bad.sigma = -1.0;
  const auto refused = PriceLanes(c, bad, ok, 1);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().price.contract, pricer::ContractError::kVolatility);
  const auto bad_run = PriceLanes(c, kMarket, {.seed = kSeed, .paths = 63, .batches = 2}, 1);
  ASSERT_FALSE(bad_run.has_value());
  EXPECT_EQ(bad_run.error().price.run, pricer::RunError::kPaths);
  for (const std::size_t lanes : {std::size_t{0}, std::size_t{3}, MaxLanes() * 2}) {
    const auto wrong = PriceLanes(c, kMarket, ok, lanes);
    ASSERT_FALSE(wrong.has_value()) << lanes;
    EXPECT_TRUE(wrong.error().lanes) << lanes;
  }
}

// The widths are every power of two up to the target's lanes, and the target is the one the build
// asked for: a missing ISA flag makes Highway fall back to a narrower target without complaint.
TEST(SimdPrices, LaneWidthsCoverTheTarget) {
  const auto widths = LaneWidths();
  ASSERT_FALSE(widths.empty());
  EXPECT_EQ(widths.front(), 1U);
  for (std::size_t i = 1; i < widths.size(); ++i) EXPECT_EQ(widths[i], 2 * widths[i - 1]);
  EXPECT_EQ(widths.back(), MaxLanes());
  EXPECT_EQ(MaxLanes(), testing::kTargetLanes);
  const std::string_view target = build_info::simd_target;
  std::size_t expected = 2;  // baseline: NEON on arm64, SSE2 on x86-64
  if (target == "avx2") expected = 4;
  if (target == "avx512") expected = 8;
  if (target == "scalar") expected = 1;
  EXPECT_EQ(MaxLanes(), expected) << "target " << TargetName() << " for " << target;
  std::cout << "simd target " << TargetName() << " (" << target << "), " << MaxLanes()
            << " double lanes\n";
}

}  // namespace
}  // namespace aadp::simd
