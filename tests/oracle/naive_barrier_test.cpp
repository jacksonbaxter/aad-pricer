// The failing test: the naive pathwise delta on a hard indicator is wrong, the tolerance rule
// must reject it, and it must be wrong for the known reason.
//
// A hard indicator has zero derivative almost everywhere, so differentiating the path loses the
// term from paths that cross the barrier as the spot moves. The estimator then converges, with
// more paths, to E[e^{-rT} 1{survive, S_T > K} S_T / S_0], which the quadrature tool computes as
// the naive limit. Agreeing with that limit is what proves the reason.
//
// Choices fixed before measuring:
// - N = 2^21 paths in 64 batches, seed 424242, a size the suite can afford on every run. Smaller
//   than the plan's, because the failure is 0.4 wide and this is not a precision test.
// - K and B as the plan's, so the rule here is the rule there.
// - CRN differences on the same hard-indicator kernel, h = 1, 0.5, 0.25. A difference does see a
//   path cross, so it has no such bias, at the price of variance growing as 1/h. Its b comes from
//   the plan's two-h rule on the two smallest steps.
//
// A non-Release build runs 2^15 paths as a smoke test: it asserts only what that size can show.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "mc/batch_runner.hpp"
#include "mc/gbm_kernels.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/finite_difference.hpp"
#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"
#include "runner.hpp"
#include "support/dual.hpp"
#include "support/mc_support.hpp"

#ifndef AADP_SMOKE_SIZE
#error "cmake/area_oracle.cmake sets AADP_SMOKE_SIZE"
#endif

namespace aadp {
namespace {

using test::ActiveMarket;
using test::Dual;
using test::ReadGreek;

constexpr bool kSmoke = AADP_SMOKE_SIZE != 0;
constexpr std::uint64_t kPaths = kSmoke ? std::uint64_t{1} << 15 : std::uint64_t{1} << 21;
constexpr std::uint32_t kBatches = 64;
constexpr std::uint64_t kSeed = 424242;
constexpr std::array<double, 3> kSteps = {1.0, 0.5, 0.25};

constexpr MarketParams<double> kMarket{
    .spot = 100.0, .sigma = 0.30, .r = 0.10, .q = 0.0, .maturity = 0.2};

struct Row {
  double barrier;
  const char* value_id;
  const char* delta_id;
  const char* naive_id;
};

constexpr auto kRows = std::to_array<Row>({
    {.barrier = 85,
     .value_id = "DB085.value",
     .delta_id = "DB085.delta",
     .naive_id = "DB085.naive"},
    {.barrier = 90,
     .value_id = "DB090.value",
     .delta_id = "DB090.delta",
     .naive_id = "DB090.naive"},
    {.barrier = 95, .value_id = "O5", .delta_id = "O6", .naive_id = "DB095.naive"},
    {.barrier = 97,
     .value_id = "DB097.value",
     .delta_id = "DB097.delta",
     .naive_id = "DB097.naive"},
    {.barrier = 99,
     .value_id = "DB099.value",
     .delta_id = "DB099.delta",
     .naive_id = "DB099.naive"},
});

ReferenceTable LoadReferences() {
  const std::ifstream file("references/oracle_values.csv", std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return *DecodeReferences(text.str());
}

// value, naive delta, FD(1), FD(0.5), FD(0.25)
constexpr std::size_t kStats = 5;
using Table = std::vector<std::array<double, kStats>>;

Table RunHard(double barrier) {
  const Contract contract{.product = Product::kDownOutHard,
                          .type = OptionType::kCall,
                          .strike = 100.0,
                          .barrier = barrier,
                          .steps = 50,
                          .include_spot = false};
  const MarketParams<Dual> active = ActiveMarket(kMarket);
  const RunConfig config{
      .seed = kSeed, .paths = kPaths, .batches = kBatches, .steps = contract.steps, .threads = 8};
  return RunBatches<kStats>(
      config, [&](std::span<const double> u, std::array<double, kStats>& out) {
        const Dual path = DiscountedPayoff<Dual>(contract, active, u);
        out[0] = path.value;
        out[1] = ReadGreek(path, Greek::kDelta);
        for (std::size_t i = 0; i < kSteps.size(); ++i) {
          out[2 + i] = PathCentralDifference(contract, kMarket, Greek::kDelta, kSteps[i], u);
        }
      });
}

std::vector<double> Column(const Table& table, std::size_t index) {
  std::vector<double> column;
  for (const auto& row : table) column.push_back(row[index]);
  return column;
}

double PlanCritical() {
  return CriticalValue(kFamilyWiseAlpha, oracle_runner::PlanTemplate().size(), kBatches);
}

TEST(NaiveBarrier, RejectedAtEveryHForTheKnownReason) {
  const ReferenceTable references = LoadReferences();
  const double critical = PlanCritical();
  constexpr double kNoCeiling = 1.0;
  for (const Row& row : kRows) {
    const Table table = RunHard(row.barrier);
    const auto naive = Column(table, 1);
    const double truth = references.Find(row.delta_id)->value;
    const double limit = references.Find(row.naive_id)->value;

    const ToleranceResult against_truth = CheckAgainstReference(
        naive, truth,
        {.b = references.Find(row.delta_id)->b, .se_ceiling = kNoCeiling, .critical = critical});
    const ToleranceResult against_limit = CheckAgainstReference(
        naive, limit,
        {.b = references.Find(row.naive_id)->b, .se_ceiling = kNoCeiling, .critical = critical});
    const double gap = std::fabs(against_truth.estimate - truth);
    const double margin = gap / against_truth.bound;
    std::printf(
        "H=%.0f naive=%.6f se=%.2e true=%.6f gap=%.6f bound=%.6f margin=%.1f limit=%.6f "
        "against limit=%s\n",
        row.barrier, against_truth.estimate, against_truth.standard_error, truth, gap,
        against_truth.bound, margin, limit, against_limit.passed() ? "pass" : "FAIL");

    // At full size the rule rejects every row. Smoke size cannot resolve H=85's 0.011.
    if (!kSmoke || row.barrier >= 90) {
      EXPECT_FALSE(against_truth.passed()) << row.barrier;
      EXPECT_STREQ(against_truth.failed_condition(), "distance") << row.barrier;
      EXPECT_LT(against_truth.estimate, truth) << "the lost term is positive here";
    }
    EXPECT_TRUE(against_limit.passed()) << row.barrier;
    if (row.barrier == 95.0) {
      EXPECT_NEAR(limit, 0.39101, 5.1e-6);
      EXPECT_NEAR(truth, 0.7978141916, 1e-9);
      EXPECT_GT(margin, kSmoke ? 10.0 : 100.0);
    }

    // The kernel prices correctly. Only its derivative is wrong.
    const ToleranceResult value = CheckAgainstReference(
        Column(table, 0), references.Find(row.value_id)->value,
        {.b = references.Find(row.value_id)->b, .se_ceiling = 1.0, .critical = critical});
    EXPECT_TRUE(value.passed()) << row.barrier;
  }
}

TEST(NaiveBarrier, CrnDifferencesOnTheHardIndicatorMoveToTheTruth) {
  const ReferenceTable references = LoadReferences();
  const double critical = PlanCritical();
  for (const Row& row : kRows) {
    const Table table = RunHard(row.barrier);
    const double truth = references.Find(row.delta_id)->value;
    const double naive = BatchMeansStatistics(Column(table, 1)).mean;

    std::vector<double> differences;
    for (const auto& batch : table) differences.push_back(batch[3] - batch[4]);
    const BatchStatistics two_h = BatchMeansStatistics(differences);
    const double b =
        oracle_runner::kFdBFactor * (std::fabs(two_h.mean) + critical * two_h.standard_error) / 3.0;
    const ToleranceResult finest = CheckAgainstReference(
        Column(table, 4), truth, {.b = b, .se_ceiling = 1.0, .critical = critical});

    std::printf("H=%.0f true=%.6f naive=%.6f", row.barrier, truth, naive);
    for (std::size_t i = 0; i < kSteps.size(); ++i) {
      const BatchStatistics fd = BatchMeansStatistics(Column(table, 2 + i));
      std::printf("  h=%.2f: %.6f (se %.1e)", kSteps[i], fd.mean, fd.standard_error);
    }
    std::printf("\n");

    if (kSmoke) continue;
    EXPECT_TRUE(finest.passed()) << row.barrier << ": " << finest.estimate << " bound "
                                 << finest.bound;
    EXPECT_LT(std::fabs(finest.estimate - truth), std::fabs(naive - truth)) << row.barrier;
  }
}

}  // namespace
}  // namespace aadp
