// The 12 knock-in rows of stage 8 (docs/NOTES.md, "Stage 8") at a development seed: the production
// tape against references/oracle_values.csv by CheckAgainstReference with the plan's c and the
// reference's b, and no ceiling. Deterministic. Builds other than release run a few paths and only
// check that every row is finite.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "oracle/plan.hpp"
#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/runner.hpp"
#include "production.hpp"
#include "runner.hpp"

#ifndef AADP_SMOKE_SIZE
#define AADP_SMOKE_SIZE 1
#endif

namespace aadp::oracle_runner {
namespace {

constexpr bool kSmoke = AADP_SMOKE_SIZE != 0;
// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261128;
constexpr std::uint64_t kPaths = kSmoke ? (1U << 12) : (1U << 18);
constexpr std::uint32_t kBatches = 64;

std::string ReadFile(const std::string& path) {
  const std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

TEST(KnockInRows, TwelveRowsAgainstTheirReferences) {
  const auto references = DecodeReferences(ReadFile("references/oracle_values.csv"));
  ASSERT_TRUE(references.has_value());
  const double critical = CriticalValue(0.01, PlanTemplate().size(), kBatches);
  const pricer::RunConfig config{.seed = kSeed, .paths = kPaths, .batches = kBatches, .threads = 4};
  std::size_t rows = 0;
  for (const PlanTemplateRow& row : PlanTemplate()) {
    const auto instrument = ResolveInstrument(row.instrument);
    if (!instrument) FAIL() << row.id;
    if (instrument->kind != InstrumentKind::kKnockIn) continue;
    ++rows;
    ASSERT_TRUE(instrument->gbm.knock_in) << row.id;
    const Reference* reference = references->Find(row.reference);
    ASSERT_NE(reference, nullptr) << row.id;
    EXPECT_EQ(reference->parameters, instrument->parameters) << row.id;
    const auto batches = GbmTapeBatches(instrument->gbm, config);
    ASSERT_TRUE(batches.has_value()) << row.id << ": " << batches.error();
    const auto greek = DecodeGreek(row.greek);
    if (!greek) FAIL() << row.id;
    const ToleranceResult r =
        CheckAgainstReference((*batches)[static_cast<std::size_t>(*greek)], reference->value,
                              {.b = reference->b,
                               .se_ceiling = std::numeric_limits<double>::infinity(),
                               .critical = critical});
    if (kSmoke) {
      EXPECT_TRUE(std::isfinite(r.estimate) && std::isfinite(r.standard_error)) << row.id;
      continue;
    }
    EXPECT_TRUE(r.passed()) << row.id << ": estimate " << r.estimate << " reference " << r.reference
                            << " se " << r.standard_error << " bound " << r.bound << " distance/se "
                            << std::fabs(r.estimate - r.reference) / r.standard_error;
  }
  EXPECT_EQ(rows, 12U);
}

}  // namespace
}  // namespace aadp::oracle_runner
