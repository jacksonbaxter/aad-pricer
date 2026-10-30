// N-thread equals one-thread, bit for bit, on every kernel. Small enough to run under TSan.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "mc/batch_runner.hpp"
#include "mc/gbm_kernels.hpp"
#include "oracle/finite_difference.hpp"
#include "support/dual.hpp"
#include "support/mc_support.hpp"

namespace aadp {
namespace {

using test::ActiveMarket;
using test::Dual;
using test::ReadGreek;

constexpr MarketParams<double> kMarket{100.0, 0.30, 0.10, 0.02, 0.5};
constexpr std::size_t kStats = 7;

using Table = std::vector<std::array<double, kStats>>;

// Value, five Dual Greeks, one finite difference, so every code path a measured run takes.
Table RunStub(const Contract& c, std::uint64_t paths, std::uint32_t threads) {
  const RunConfig config{20261030, paths, 8, c.steps, threads};
  return RunBatches<kStats>(config,
                            [&](std::span<const double> u, std::array<double, kStats>& out) {
                              const Dual dual = DiscountedPayoff<Dual>(c, ActiveMarket(kMarket), u);
                              out[0] = dual.value;
                              out[1] = ReadGreek(dual, Greek::kDelta);
                              out[2] = ReadGreek(dual, Greek::kVega);
                              out[3] = ReadGreek(dual, Greek::kRho);
                              out[4] = ReadGreek(dual, Greek::kDividendRho);
                              out[5] = ReadGreek(dual, Greek::kTheta);
                              out[6] = PathCentralDifference(c, kMarket, Greek::kVega, 1e-4, u);
                            });
}

bool BitIdentical(const Table& a, const Table& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0;
}

TEST(ThreadInvariance, OneTwoAndFourThreadsAreBitIdentical) {
  const std::vector<Contract> contracts = {
      {Product::kEuropean, OptionType::kCall, 100.0, 0.0, 1, false},
      {Product::kAsianArithmetic, OptionType::kCall, 100.0, 0.0, 12, true},
      {Product::kAsianGeometric, OptionType::kCall, 100.0, 0.0, 12, false},
      {Product::kDownOutHard, OptionType::kCall, 100.0, 95.0, 12, false},
      {Product::kDownOutOneStep, OptionType::kCall, 100.0, 95.0, 12, false},
      {Product::kDownOutBridge, OptionType::kCall, 100.0, 95.0, 12, false},
  };
  // Over three blocks per batch, so blocks are really taken out of order, plus a part block.
  constexpr std::uint64_t kPaths = 8 * (3 * kPathsPerBlock + 17);
  for (const Contract& c : contracts) {
    const Table one = RunStub(c, kPaths, 1);
    EXPECT_TRUE(BitIdentical(one, RunStub(c, kPaths, 2))) << static_cast<int>(c.product);
    EXPECT_TRUE(BitIdentical(one, RunStub(c, kPaths, 4))) << static_cast<int>(c.product);
  }
}

// A check that cannot fail proves nothing: changing the data must change the bytes.
TEST(ThreadInvariance, ComparisonSeesAChange) {
  const Contract c{Product::kEuropean, OptionType::kCall, 100.0, 0.0, 1, false};
  EXPECT_FALSE(BitIdentical(RunStub(c, 8 * 16, 1), RunStub(c, 8 * 17, 1)));
}

TEST(BatchRunner, BatchMeansPartitionThePathsInOrder) {
  // Each path's single statistic is its first uniform, so a batch mean can be recomputed here.
  constexpr std::uint64_t kPaths = 4 * 2500;
  const RunConfig config{5, kPaths, 4, 1, 3};
  const auto table = RunBatches<1>(
      config, [](std::span<const double> u, std::array<double, 1>& out) { out[0] = u[0]; });
  ASSERT_EQ(table.size(), 4U);
  std::uint64_t path = 0;
  for (std::size_t batch = 0; batch < 4; ++batch) {
    // Block-then-total summation, as the runner does it.
    double total = 0.0;
    for (std::uint64_t done = 0; done < 2500;) {
      double block = 0.0;
      const std::uint64_t size = std::min<std::uint64_t>(kPathsPerBlock, 2500 - done);
      for (std::uint64_t i = 0; i < size; ++i) block += PathUniform(5, path++, 0);
      total += block;
      done += size;
    }
    EXPECT_EQ(table[batch][0], total / 2500.0) << batch;
  }
}

}  // namespace
}  // namespace aadp
