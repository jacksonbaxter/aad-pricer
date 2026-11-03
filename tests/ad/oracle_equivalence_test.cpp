// Check 7: the run rows of references/comparison_plan.csv with the tape as the estimator in place
// of the forward-mode Dual, on the production pricer, at a development seed. Run 2 of the oracle
// plan (stage 8) uses the tape; this shows that on every run row the tape's estimate is the
// Dual's to within round-off, so the estimator swap changes no verdict by more than that.
// references/ is read, never written.
//
// Bound, stated before measuring, per batch and per Greek. Both batch means are sums over the
// batch's paths of the same pathwise terms (products of the same partials, bounds.hpp), divided
// by the same n. A term of path i passes at most
//   Dual  2 T_i on the path, then the block's P additions and the batch's blocks additions;
//   tape  T_i + O_i on the path, P O_i more where the set-up adjoints accumulate over the block
//         (each set-up node takes at most O_i contributions per path), and the blocks additions;
// so m_i = 2 T_i + (P + 1) O_i + P + 2 blocks + 4 covers both, and
//   |tape - dual| <= (1 / n) sum_i 2 gamma(m_i) A_i (1 + 2 gamma(m_i)) + n 2^-1070,
// A_i the path's absolute gradient. The value column must be Price()'s, bit for bit. Per path,
// on the same paths, check 1's bound must also hold.
//
// Size: the rows' own B, and N / 64 of their N in a release build (N / 1024 otherwise). The
// identity is per path, so N changes the evidence only by how many paths it covers.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ad/gradient.hpp"
#include "bounds.hpp"
#include "cases.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/plan.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/dual_ops.hpp"  // IWYU pragma: keep (tests/pricer/: the Dual overloads, by ADL)
#include "pricer/from_plan.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "support/dual.hpp"

#ifndef AADP_SMOKE_SIZE
#error "cmake/area_pricer.cmake sets AADP_SMOKE_SIZE"
#endif

namespace aadp::ad {
namespace {

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261115;
constexpr std::uint64_t kDivisor = AADP_SMOKE_SIZE ? 1024 : 64;
constexpr std::uint32_t kThreads = 8;

// Per path: value, the Dual's five Greeks (theta as -dV/dT), five bound terms, and a count of
// paths outside check 1's bound.
constexpr std::size_t kDualOffset = 1;
constexpr std::size_t kBoundOffset = 1 + kInputs;
constexpr std::size_t kFailures = 1 + (2 * kInputs);
constexpr std::size_t kStats = kFailures + 1;

class DualPathFn {
 public:
  DualPathFn(const pricer::Contract& c, const pricer::Market<double>& m, std::uint64_t block,
             std::uint64_t blocks)
      : contract_(c),
        dual_model_(testing::DualMarket(m), c.steps),
        tape_(c, m),
        block_(static_cast<double>(block)),
        blocks_(static_cast<double>(blocks)) {}

  void operator()(const pricer::PathUniforms& u, std::array<double, kStats>& stats) {
    stats = {};
    const test::Dual dual = pricer::DiscountedPayoff<test::Dual>(contract_, dual_model_, u);
    const PathGradient::PathResult tape = tape_.Evaluate(u);
    stats[0] = dual.value;
    const double m = (2.0 * static_cast<double>(tape.statements)) +
                     ((block_ + 1.0) * static_cast<double>(tape.operands)) + block_ +
                     (2.0 * blocks_) + 4.0;
    const double gamma = testing::Gamma(m);
    for (std::size_t i = 0; i < kInputs; ++i) {
      const double sign = i == kMaturityInput ? -1.0 : 1.0;
      stats[kDualOffset + i] = sign * dual.tangent[i];
      stats[kBoundOffset + i] = 2.0 * gamma * tape.absolute[i] * (1.0 + (2.0 * gamma));
      const double path_bound =
          testing::AgreementBound(tape.absolute[i], tape.statements, tape.operands);
      if (!(std::fabs(tape.gradient[i] - dual.tangent[i]) <= path_bound)) stats[kFailures] += 1;
    }
  }

 private:
  pricer::Contract contract_;
  pricer::GbmModel<test::Dual> dual_model_;
  PathGradient tape_;
  double block_;   // P, paths per block
  double blocks_;  // blocks per batch
};

std::string ReadFile(const char* path) {
  const std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

std::size_t GreekColumn(aadp::Greek greek) { return static_cast<std::size_t>(greek); }

TEST(OracleEquivalence, TapeEstimatesEqualTheDualOnEveryRunRow) {
  const auto plan = DecodePlan(ReadFile("references/comparison_plan.csv"));
  ASSERT_TRUE(plan.has_value());
  using Key = std::tuple<std::string, std::uint64_t, std::uint32_t>;
  std::map<Key, std::vector<const PlanRow*>> groups;
  for (const PlanRow& row : plan->rows) {
    if (row.status != RowStatus::kRun) continue;
    ASSERT_NE(row.seed, kSeed);
    groups[{row.instrument, row.paths / kDivisor, row.batches}].push_back(&row);
  }
  std::size_t rows = 0;
  double worst = 0.0;
  for (const auto& [key, members] : groups) {
    const auto& [text, paths, batches] = key;
    const auto instrument = DecodeInstrument(text);
    ASSERT_TRUE(instrument.has_value()) << text;
    const pricer::Contract contract = pricer::FromPlan(instrument->contract);
    const pricer::Market<double> market = pricer::FromPlan(instrument->market);
    ASSERT_EQ(pricer::Validate(contract, market), pricer::ContractError::kNone) << text;
    const pricer::RunConfig config{
        .seed = kSeed, .paths = paths, .batches = batches, .threads = kThreads};
    ASSERT_EQ(paths % batches, 0U) << text;

    const auto tape = PriceWithGradient(contract, market, config);
    const auto price = pricer::Price(contract, market, config);
    ASSERT_TRUE(tape.has_value() && price.has_value()) << text;
    const std::uint64_t block = std::min(pricer::kBlockPaths, paths / batches);
    const std::uint64_t blocks =
        ((paths / batches) + pricer::kBlockPaths - 1) / pricer::kBlockPaths;
    const pricer::PathShape shape{.steps = contract.steps,
                                  .crossing = pricer::NeedsCrossingUniforms(contract)};
    const auto dual = pricer::RunBatches<kStats>(
        config, shape, [&] { return DualPathFn(contract, market, block, blocks); });
    const auto n = static_cast<double>(paths / batches);

    double failures = 0.0;
    for (const auto& row : dual) failures += row[kFailures];
    EXPECT_EQ(failures, 0.0) << text << ": paths outside check 1's bound";
    for (std::size_t b = 0; b < batches; ++b) {
      EXPECT_EQ(tape->value.batch_means[b], price->batch_means[b]) << text << ", batch " << b;
      EXPECT_EQ(tape->value.batch_means[b], dual[b][0]) << text << ", batch " << b;
    }
    for (const PlanRow* row : members) {
      const auto greek = DecodeGreek(row->greek);
      ASSERT_TRUE(greek.has_value()) << row->id;
      rows += 1;
      if (*greek == aadp::Greek::kValue) continue;
      const std::size_t i = GreekColumn(*greek) - 1;
      double row_worst = 0.0;
      for (std::size_t b = 0; b < batches; ++b) {
        const double t = tape->greeks[i].batch_means[b];
        const double d = dual[b][kDualOffset + i];
        const double bound = (dual[b][kBoundOffset + i] * (1.0 + 0x1p-50)) + (n * 0x1p-1070) +
                             (0x1p-52 * (std::fabs(t) + std::fabs(d)));
        ASSERT_LE(std::fabs(t - d), bound) << row->id << ", batch " << b;
        if (t != d) row_worst = std::max(row_worst, std::fabs(t - d) / bound);
      }
      std::vector<double> dual_means;
      for (std::size_t b = 0; b < batches; ++b) dual_means.push_back(dual[b][kDualOffset + i]);
      std::printf("measured, %-18s tape %.10g dual %.10g, max batch |diff| / bound %.3g\n",
                  row->id.c_str(), tape->greeks[i].mean, BatchMeansStatistics(dual_means).mean,
                  row_worst);
      worst = std::max(worst, row_worst);
    }
  }
  EXPECT_EQ(rows, 90U) << "the plan's run rows";
  std::printf(
      "measured: %zu run rows in %zu groups at N / %llu, seed %llu: value columns bit-identical "
      "to Price; worst Greek batch |tape - dual| / bound %.3g%s\n",
      rows, groups.size(), static_cast<unsigned long long>(kDivisor),
      static_cast<unsigned long long>(kSeed), worst,
      AADP_SMOKE_SIZE ? " (reduced size, not a result)" : "");
}

}  // namespace
}  // namespace aadp::ad
