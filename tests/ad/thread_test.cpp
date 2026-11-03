// Thread-local tapes (EVIDENCE B10), small enough for the tsan preset (label tsan). Each worker
// of RunBatches builds its own tape, so PriceWithGradient must give the same bits for any thread
// count, as Price does; and tapes on separate threads must not see each other. The merge across
// workers itself (B11, B14) is stage 2b's to document and measure; this only shows the tapes are
// independent and race-free.
#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "ad/gradient.hpp"
#include "cases.hpp"
#include "pricer/draws.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::ad {
namespace {

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261116;
constexpr std::uint32_t kBatches = 4;
constexpr std::uint64_t kPaths = kBatches * ((2 * pricer::kBlockPaths) + 5);

bool Same(const pricer::Estimate& a, const pricer::Estimate& b) {
  if (a.batch_means.size() != b.batch_means.size()) return false;
  for (std::size_t i = 0; i < a.batch_means.size(); ++i) {
    if (std::bit_cast<std::uint64_t>(a.batch_means[i]) !=
        std::bit_cast<std::uint64_t>(b.batch_means[i])) {
      return false;
    }
  }
  return true;
}

TEST(TapeThreads, PriceWithGradientIsBitIdenticalForOneTwoAndThreeThreads) {
  for (const testing::Case& c : testing::AllCases(8)) {
    std::array<GradientEstimate, 3> runs;
    for (std::uint32_t t = 1; t <= 3; ++t) {
      const auto estimate =
          PriceWithGradient(c.contract, testing::kMarket,
                            {.seed = kSeed, .paths = kPaths, .batches = kBatches, .threads = t});
      ASSERT_TRUE(estimate.has_value()) << c.name;
      runs[t - 1] = *estimate;
    }
    for (std::size_t t = 1; t < 3; ++t) {
      EXPECT_TRUE(Same(runs[0].value, runs[t].value)) << c.name << ", " << t + 1 << " threads";
      for (std::size_t g = 0; g < kInputs; ++g) {
        EXPECT_TRUE(Same(runs[0].greeks[g], runs[t].greeks[g]))
            << c.name << ", greek " << g << ", " << t + 1 << " threads";
      }
    }
  }
}

// Four threads, each with its own tape on a different contract, run at once; each gradient is
// the one the same work gives alone on this thread.
TEST(TapeThreads, TapesOnSeparateThreadsAreIndependent) {
  const std::vector<testing::Case> cases = testing::AllCases(12);
  constexpr std::size_t kWorkers = 4;
  constexpr std::uint64_t kPathsEach = 64;
  const auto work = [&](std::size_t which) {
    const testing::Case& c = cases[(7 * which) % cases.size()];
    PathGradient path(c.contract, testing::kMarket);
    std::vector<double> normal(c.contract.steps);
    std::vector<double> crossing(c.contract.steps);
    const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
    for (std::uint64_t p = 0; p < kPathsEach; ++p) {
      pricer::FillPath(kSeed, p, normal, crossing);
      static_cast<void>(path.Accumulate(u));
    }
    return path.SweepSetup();
  };
  std::array<std::array<double, kInputs>, kWorkers> alone{};
  for (std::size_t w = 0; w < kWorkers; ++w) alone[w] = work(w);
  std::array<std::array<double, kInputs>, kWorkers> together{};
  {
    std::vector<std::jthread> threads;
    threads.reserve(kWorkers);
    for (std::size_t w = 0; w < kWorkers; ++w) {
      threads.emplace_back([&, w] { together[w] = work(w); });
    }
  }
  for (std::size_t w = 0; w < kWorkers; ++w) {
    for (std::size_t i = 0; i < kInputs; ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(alone[w][i]),
                std::bit_cast<std::uint64_t>(together[w][i]))
          << "worker " << w << ", input " << i;
    }
  }
}

}  // namespace
}  // namespace aadp::ad
