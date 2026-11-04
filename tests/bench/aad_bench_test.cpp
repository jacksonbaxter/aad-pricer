// The AAD benchmark's in-run checks (bench/aad_checks.hpp): they pass on every workload, and each
// fails, marking the workload invalid, when one variant's result is broken.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "aad_checks.hpp"
#include "aad_workloads.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::bench::aad {
namespace {

constexpr std::uint64_t kScale = 500;

// ConfigFor, where the test needs it to accept.
pricer::RunConfig Config(const Workload& w, std::uint64_t divisor, std::uint32_t threads) {
  const auto config = ConfigFor(w, divisor, threads);
  if (!config) throw std::logic_error("no config");
  return *config;
}

VariantResult& Mutable(WorkloadReport& report, Variant variant) {
  for (VariantResult& r : report.results) {
    if (r.variant == variant) return r;
  }
  throw std::logic_error("no such variant");
}

const WorkloadReport& AsianReport() {
  static const WorkloadReport report = [] {
    const Workload* w = FindWorkload("aad4-asian12");
    return Verify(*w, Config(*w, kScale, 1));
  }();
  return report;
}

const WorkloadReport& LwReport() {
  static const WorkloadReport report = [] {
    const Workload* w = FindWorkload("lw-worst");
    return Verify(*w, Config(*w, 1, 1));
  }();
  return report;
}

// Only the named check fails, and the workload is marked invalid in its section.
void ExpectOnlyFailure(const WorkloadReport& report, std::string_view name) {
  const std::vector<CheckResult> checks = CheckResults(report);
  for (const CheckResult& c : checks) {
    EXPECT_EQ(c.passed, c.name != name) << c.name << ": " << c.detail;
  }
  WorkloadReport marked = report;
  marked.checks = checks;
  EXPECT_FALSE(marked.valid());
  const std::string section = RenderSection(marked);
  EXPECT_NE(section.find("\nvalid no\n"), std::string::npos);
  EXPECT_NE(section.find("check " + std::string(name) + " fail"), std::string::npos) << section;
}

TEST(AadChecks, EveryCheckPassesOnEveryWorkloadAtASmallSize) {
  for (const Workload& w : Workloads()) {
    const WorkloadReport report = Verify(w, Config(w, kScale, 3));
    const bool lanes = HasVariant(w, Variant::kTapeLanes);
    EXPECT_EQ(report.checks.size(), (w.hand ? 5U : 4U) + (lanes ? 2U : 0U)) << w.id;
    EXPECT_EQ(lanes, w.kind == Kind::kPricer && !w.second_order) << w.id;
    if (lanes) {
      EXPECT_EQ(report.lane_tape.lanes, LaneCount()) << w.id;
      EXPECT_GT(report.lane_tape.bytes_per_path, 0.0) << w.id;
      EXPECT_EQ(report.Result(Variant::kTapeLanes)->fallback_groups, 0U) << w.id;
    }
    for (const CheckResult& c : report.checks) {
      EXPECT_TRUE(c.passed) << w.id << " " << c.name << ": " << c.detail;
    }
    EXPECT_TRUE(report.valid()) << w.id;
    EXPECT_NE(RenderSection(report).find("\nvalid yes\n"), std::string::npos) << w.id;
    EXPECT_GT(report.tape.bytes_per_path, 0U) << w.id;
    EXPECT_GT(report.nonzero_fraction, 0.0) << w.id;
    EXPECT_LE(report.nonzero_fraction, 1.0) << w.id;
  }
}

// At 1/500 of its paths the barrier's delta gap between a bump that moved the smoothing with the
// spot and the tape, which holds it, is under 4 SEs of the difference; at 1/25 it is near 20.
TEST(AadChecks, TheBarrierBumpsDifferenceTheFunctionTheTapeDifferentiates) {
  const Workload* w = FindWorkload("aad1-barrier");
  const WorkloadReport report = Verify(*w, Config(*w, 25, 4));
  for (const CheckResult& c : report.checks) {
    EXPECT_TRUE(c.passed) << c.name << ": " << c.detail;
  }
}

TEST(AadChecks, ABumpWithTheWrongThetaSignFails) {
  WorkloadReport report = AsianReport();
  pricer::Estimate& theta = Mutable(report, Variant::kBumpCentral).greeks[4];
  for (double& x : theta.batch_means) x = -x;
  theta.mean = -theta.mean;
  ExpectOnlyFailure(report, "central-vs-tape");
}

TEST(AadChecks, AForwardBumpOffByItsTruncationFails) {
  WorkloadReport report = AsianReport();
  // A forward step one thousand times too large leaves a bias far above its allowance.
  pricer::Estimate& delta = Mutable(report, Variant::kBumpForward).greeks[0];
  for (double& x : delta.batch_means) x += 1e-3;
  delta.mean += 1e-3;
  ExpectOnlyFailure(report, "forward-vs-tape");
}

TEST(AadChecks, AHandAdjointOffByAMillionUlpsFails) {
  WorkloadReport report = AsianReport();
  double& vega = Mutable(report, Variant::kHand).greeks[1].batch_means[7];
  vega *= 1.0 + 0x1p-32;
  ExpectOnlyFailure(report, "hand-vs-tape");
}

TEST(AadChecks, ASkipThatDropsAStatementFails) {
  WorkloadReport report = AsianReport();
  double& rho = Mutable(report, Variant::kTapeSkip).greeks[2].batch_means[3];
  rho = std::nextafter(rho, 0.0);
  ExpectOnlyFailure(report, "skip-equals-tape");
}

TEST(AadChecks, AValueThatIsNotPricesFails) {
  WorkloadReport report = AsianReport();
  double& value = Mutable(report, Variant::kTapeRecord).value.batch_means[0];
  value = std::nextafter(value, 0.0);
  ExpectOnlyFailure(report, "value-bits");
}

TEST(AadChecks, AVectorTapeGreekOneUlpOffFails) {
  WorkloadReport report = AsianReport();
  double& vega = Mutable(report, Variant::kTapeLanes).greeks[1].batch_means[5];
  vega = std::nextafter(vega, 0.0);
  ExpectOnlyFailure(report, "lanes-gradient-bits");
}

TEST(AadChecks, AVectorTapeSeOneUlpOffFails) {
  WorkloadReport report = AsianReport();
  double& se = Mutable(report, Variant::kTapeLanes).greeks[4].standard_error;
  se = std::nextafter(se, 0.0);
  ExpectOnlyFailure(report, "lanes-gradient-bits");
}

// A fallback keeps the scalar tape's bits, so only the count can show it.
TEST(AadChecks, AFallbackInAPricerKernelFailsAndIsReported) {
  WorkloadReport report = AsianReport();
  VariantResult& lanes = Mutable(report, Variant::kTapeLanes);
  lanes.fallback_groups = 3;
  lanes.fallback_paths = 9;
  ExpectOnlyFailure(report, "lanes-fallback-zero");
  WorkloadReport marked = report;
  marked.checks = CheckResults(report);
  const std::string section = RenderSection(marked);
  EXPECT_NE(section.find("\nlane_fallback_groups 3\n"), std::string::npos) << section;
  EXPECT_NE(section.find("\nlane_fallback_paths 9\n"), std::string::npos);
  EXPECT_NE(section.find("3 groups, 9 paths fell back to the scalar tape"), std::string::npos);
}

TEST(AadChecks, ALanePrimalValueThatIsNotPricesFails) {
  WorkloadReport report = AsianReport();
  double& value = Mutable(report, Variant::kPriceLanes).value.batch_means[2];
  value = std::nextafter(value, 0.0);
  ExpectOnlyFailure(report, "value-bits");
}

TEST(AadChecks, TheLaxWendroffChecksPassAndCatchAWrongInput) {
  for (const CheckResult& c : LwReport().checks) {
    EXPECT_TRUE(c.passed) << c.name << ": " << c.detail;
  }
  WorkloadReport hand = LwReport();
  Mutable(hand, Variant::kHand).gradient[37] *= 1.0 + 0x1p-30;
  ExpectOnlyFailure(hand, "hand-vs-tape");
  WorkloadReport bump = LwReport();
  // The allowance there is a few times 1e-6: a quotient of J with round-off alone.
  Mutable(bump, Variant::kBumpCentral).gradient[0] += 1e-4;
  ExpectOnlyFailure(bump, "central-vs-tape");
}

TEST(AadWorkloads, RefusesAScaleThatSplitsABatch) {
  const Workload* w = FindWorkload("aad1-barrier");
  EXPECT_FALSE(ConfigFor(*w, 7, 1).has_value());
  EXPECT_FALSE(ConfigFor(*w, 0, 1).has_value());
  EXPECT_EQ(Config(*w, 1, 8).paths, 500'000U);
  EXPECT_EQ(Config(*w, 1, 8).threads, 1U);
  EXPECT_EQ(Config(*FindWorkload("aad3-barrier-mt"), 1, 8).threads, 8U);
}

}  // namespace
}  // namespace aadp::bench::aad
