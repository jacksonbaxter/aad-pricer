// Check 2: the pricer's result does not depend on the thread count, bit for bit, and equals a
// plain single-threaded loop over the same blocks in the same order. Small enough for TSan, which
// runs it under the tsan preset (label tsan).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::pricer {
namespace {

constexpr Market<double> kMarket{
    .spot = 100.0, .sigma = 0.30, .r = 0.10, .q = 0.02, .maturity = 0.5};
// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261107;
constexpr std::uint32_t kBatches = 8;
// Three blocks and a part block per batch, so workers really do take blocks out of order.
constexpr std::uint64_t kPaths = kBatches * ((3 * kBlockPaths) + 17);

struct Case {
  std::string name;
  Contract contract;
};

Contract Plain(Product product, std::uint32_t steps, bool include_spot = false) {
  Contract c;
  c.product = product;
  c.strike = 100.0;
  c.steps = steps;
  c.include_spot = include_spot;
  return c;
}

Contract WithBarrier(Direction direction, Knock knock, Monitoring monitoring, Treatment treatment) {
  Contract c = Plain(Product::kBarrier, 12);
  c.type = direction == Direction::kDown ? OptionType::kCall : OptionType::kPut;
  c.barrier = {.direction = direction,
               .knock = knock,
               .monitoring = monitoring,
               .treatment = treatment,
               .level = direction == Direction::kDown ? 95.0 : 110.0,
               .smoothing_width = 0.01};
  return c;
}

std::vector<Case> Cases() {
  using enum Direction;
  using enum Knock;
  using enum Monitoring;
  using enum Treatment;
  return {
      {.name = "european", .contract = Plain(Product::kEuropean, 1)},
      {.name = "digital", .contract = Plain(Product::kDigital, 4)},
      {.name = "arithmetic Asian", .contract = Plain(Product::kAsianArithmetic, 12, true)},
      {.name = "geometric Asian", .contract = Plain(Product::kAsianGeometric, 12)},
      {.name = "down-out bridge", .contract = WithBarrier(kDown, kOut, kContinuous, kBridge)},
      {.name = "down-out sampled crossing",
       .contract = WithBarrier(kDown, kOut, kContinuous, kHard)},
      {.name = "down-out one-step", .contract = WithBarrier(kDown, kOut, kDiscrete, kOneStep)},
      {.name = "down-out hard", .contract = WithBarrier(kDown, kOut, kDiscrete, kHard)},
      {.name = "up-out smoothed", .contract = WithBarrier(kUp, kOut, kDiscrete, kSmoothed)},
      {.name = "down-in one-step", .contract = WithBarrier(kDown, kIn, kDiscrete, kOneStep)},
      {.name = "up-in bridge", .contract = WithBarrier(kUp, kIn, kContinuous, kBridge)},
  };
}

std::vector<double> PriceWith(const Contract& c, std::uint32_t threads) {
  const auto estimate =
      Price(c, kMarket, {.seed = kSeed, .paths = kPaths, .batches = kBatches, .threads = threads});
  EXPECT_TRUE(estimate.has_value());
  return estimate ? estimate->batch_means : std::vector<double>{};
}

// The specification, with no threads: path sums per block in path order, block sums per batch in
// block order, each batch divided by its path count.
std::vector<double> SerialLoop(const Contract& c) {
  const GbmModel<double> model(kMarket, c.steps);
  std::vector<double> normal(c.steps);
  std::vector<double> crossing(NeedsCrossingUniforms(c) ? c.steps : 0);
  const PathUniforms u{.normal = normal, .crossing = crossing};
  const std::uint64_t per_batch = kPaths / kBatches;
  std::vector<double> means;
  for (std::uint64_t batch = 0; batch < kBatches; ++batch) {
    double total = 0.0;
    for (std::uint64_t first = 0; first < per_batch; first += kBlockPaths) {
      double block = 0.0;
      for (std::uint64_t i = first; i < std::min(first + kBlockPaths, per_batch); ++i) {
        FillPath(kSeed, (batch * per_batch) + i, normal, crossing);
        block += DiscountedPayoff<double>(c, model, u);
      }
      total += block;
    }
    means.push_back(total / static_cast<double>(per_batch));
  }
  return means;
}

bool BitIdentical(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::bit_cast<std::uint64_t>(a[i]) != std::bit_cast<std::uint64_t>(b[i])) return false;
  }
  return true;
}

TEST(PricerThreads, OneTwoThreeAndEightThreadsAreBitIdentical) {
  for (const Case& c : Cases()) {
    const std::vector<double> one = PriceWith(c.contract, 1);
    ASSERT_EQ(one.size(), kBatches) << c.name;
    for (const std::uint32_t threads : {2U, 3U, 8U}) {
      EXPECT_TRUE(BitIdentical(one, PriceWith(c.contract, threads))) << c.name << ", " << threads;
    }
  }
}

TEST(PricerThreads, SingleThreadRunIsThePlainLoop) {
  for (const Case& c : Cases()) {
    EXPECT_TRUE(BitIdentical(PriceWith(c.contract, 1), SerialLoop(c.contract))) << c.name;
    EXPECT_TRUE(BitIdentical(PriceWith(c.contract, 3), SerialLoop(c.contract))) << c.name;
  }
}

// A check that cannot fail proves nothing: other data must give other bytes.
TEST(PricerThreads, ComparisonSeesAChange) {
  const Contract c = Plain(Product::kEuropean, 1);
  const auto a = Price(c, kMarket, {.seed = kSeed, .paths = 16, .batches = 8, .threads = 2});
  const auto b = Price(c, kMarket, {.seed = kSeed + 1, .paths = 16, .batches = 8, .threads = 2});
  ASSERT_TRUE(a && b);
  EXPECT_FALSE(BitIdentical(a->batch_means, b->batch_means));
}

// Each worker builds its own path function once, so per-worker state such as a tape stays there.
TEST(PricerThreads, EachWorkerMakesItsOwnPathFunction) {
  for (const std::uint32_t threads : {1U, 3U, 8U}) {
    std::atomic<int> made{0};
    const auto table =
        RunBatches<1>({.seed = kSeed, .paths = kPaths, .batches = kBatches, .threads = threads},
                      {.steps = 1}, [&] {
                        made.fetch_add(1);
                        int calls = 0;
                        return [calls](const PathUniforms& u, std::array<double, 1>& out) mutable {
                          ++calls;
                          out[0] = u.normal[0];
                        };
                      });
    EXPECT_EQ(made.load(), static_cast<int>(threads));
    EXPECT_EQ(table.size(), kBatches);
  }
}

}  // namespace
}  // namespace aadp::pricer
