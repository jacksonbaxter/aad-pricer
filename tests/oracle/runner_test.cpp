// The runner's failure paths. Each is a way the suite could lie, so each has a fixture that must
// come back non-zero, for the reason named.
#include "runner.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "oracle/black_scholes.hpp"
#include "oracle/catalog.hpp"
#include "oracle/plan.hpp"
#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"

namespace aadp::oracle_runner {
namespace {

const std::string kInstrument = catalog::kVanillaO2;
const std::string kParameters(InstrumentParameters(kInstrument));

double TrueValue() {
  return BlackScholes(OptionType::kCall, 75.0, MarketParams<double>{72, 0.19, 0.09, 0.0, 1.0})
      .value;
}

ReferenceTable References(double value) {
  return *DecodeReferences("id,parameters,value,b,method,source\nREF," + kParameters + "," +
                           decode::FormatDouble(value) + ",0,closed form,made up\n");
}

std::string Header(std::size_t k, double critical) {
  return "#! K=" + std::to_string(k) +
         "\n#! alpha_F=0.01\n#! B=64\n#! critical=" + decode::FormatDouble(critical) +
         "\n#! ceiling_factor=1.65\n#! fd_b_factor=2\n#! pilot_seed=7\n#! measured_seed=9\n"
         "id,instrument,greek,reference,N,B,seed,se_ceiling,b,status\n";
}

std::string Row(const std::string& reference = "REF", const std::string& ceiling = "1",
                const std::string& b = "0") {
  return "a," + kInstrument + ",value," + reference + ",64000,64,9," + ceiling + "," + b + ",run\n";
}

struct Outcome {
  RunSummary summary;
  std::string text;
};

Outcome Execute(const std::string& text, const ReferenceTable& references, bool smoke = false) {
  const auto plan = DecodePlan(text);
  EXPECT_TRUE(plan.has_value()) << (plan ? "" : plan.error().message);
  std::ostringstream out;
  RunOptions options;
  options.threads = 2;
  options.smoke = smoke;
  RunSummary summary = RunPlan(*plan, references, options, out);
  return {summary, out.str()};
}

double Critical(std::size_t k) { return CriticalValue(0.01, k, 64); }

bool Has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

TEST(RunPlan, CleanRowPassesAndExitsZero) {
  const Outcome o = Execute(Header(1, Critical(1)) + Row(), References(TrueValue()));
  EXPECT_EQ(ExitCode(o.summary), 0) << o.text;
  EXPECT_EQ(o.summary.passed, 1U);
  EXPECT_TRUE(Has(o.text, "pass condition=none")) << o.text;
  EXPECT_TRUE(Has(o.text, "run=1 passed=1 failed=0 skipped=0 awaiting=0 K=1")) << o.text;
}

TEST(RunPlan, WrongReferenceFailsOnDistance) {
  const Outcome o = Execute(Header(1, Critical(1)) + Row(), References(TrueValue() + 5.0));
  EXPECT_EQ(ExitCode(o.summary), 1);
  EXPECT_EQ(o.summary.failed, 1U);
  EXPECT_TRUE(Has(o.text, "FAIL condition=distance")) << o.text;
}

TEST(RunPlan, NoisyRunFailsOnTheCeiling) {
  const Outcome o = Execute(Header(1, Critical(1)) + Row("REF", "1e-6"), References(TrueValue()));
  EXPECT_EQ(ExitCode(o.summary), 1);
  EXPECT_TRUE(Has(o.text, "FAIL condition=se-ceiling")) << o.text;
}

TEST(RunPlan, RowCountDifferentFromKFailsEvenWhenAllPass) {
  const Outcome o = Execute(Header(2, Critical(2)) + Row(), References(TrueValue()));
  EXPECT_EQ(o.summary.passed, 1U);
  EXPECT_EQ(ExitCode(o.summary), 1);
  EXPECT_TRUE(Has(o.text, "ERROR: row count differs")) << o.text;
}

TEST(RunPlan, StaleCriticalValueFails) {
  const Outcome o = Execute(Header(1, 4.155) + Row(), References(TrueValue()));
  EXPECT_EQ(ExitCode(o.summary), 1);
  EXPECT_TRUE(Has(o.text, "ERROR: critical value differs")) << o.text;
}

TEST(RunPlan, MissingReferenceIsASkipAndFails) {
  const Outcome o = Execute(Header(1, Critical(1)) + Row("NOPE"), References(TrueValue()));
  EXPECT_EQ(ExitCode(o.summary), 1);
  EXPECT_EQ(o.summary.skipped, 1U);
  EXPECT_TRUE(Has(o.text, "SKIPPED: reference id not found")) << o.text;
}

TEST(RunPlan, ReferenceForAnotherContractIsASkipAndFails) {
  const ReferenceTable other = *DecodeReferences(
      "id,parameters,value,b,method,source\nREF,S0=1;K=1,1.0,0,closed form,made up\n");
  const Outcome o = Execute(Header(1, Critical(1)) + Row(), other);
  EXPECT_EQ(ExitCode(o.summary), 1);
  EXPECT_TRUE(Has(o.text, "SKIPPED: reference parameters differ")) << o.text;
}

TEST(RunPlan, AllowanceDifferentFromTheReferenceFileIsASkipAndFails) {
  const Outcome o =
      Execute(Header(1, Critical(1)) + Row("REF", "1", "0.5"), References(TrueValue()));
  EXPECT_EQ(ExitCode(o.summary), 1);
  EXPECT_TRUE(Has(o.text, "SKIPPED: b differs")) << o.text;
}

TEST(RunPlan, AwaitingRowsCountInKAndAreNotRun) {
  const std::string awaiting =
      "z,svi-surface:arithmetic,d_a_d05,phase3,-,64,-,-,-,awaiting-phase3\n";
  const Outcome o = Execute(Header(2, Critical(2)) + Row() + awaiting, References(TrueValue()));
  EXPECT_EQ(ExitCode(o.summary), 0) << o.text;
  EXPECT_EQ(o.summary.awaiting, 1U);
  EXPECT_TRUE(Has(o.text, "z                  awaiting-phase3")) << o.text;
  EXPECT_TRUE(Has(o.text, "run=1 passed=1 failed=0 skipped=0 awaiting=1 K=2")) << o.text;
}

TEST(RunPlan, FiniteDifferenceRowAgreesOnASmoothKernel) {
  const std::string row = "d," + kInstrument + ",vega,FD:0.0001,64000,64,9,1,1e-5,run\n";
  const Outcome o = Execute(Header(1, Critical(1)) + row, References(TrueValue()));
  EXPECT_EQ(ExitCode(o.summary), 0) << o.text;
}

// The same row shape on the hard indicator: the difference sees crossings and Dual does not.
TEST(RunPlan, FiniteDifferenceRowCatchesTheNaiveDelta) {
  const std::string hard = "hard:S0=100;K=100;H=95;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0";
  const std::string row = "d," + hard + ",delta,FD:0.5,640000,64,9,1,1e-3,run\n";
  const Outcome o = Execute(Header(1, Critical(1)) + row, References(TrueValue()));
  EXPECT_EQ(ExitCode(o.summary), 1) << o.text;
  EXPECT_TRUE(Has(o.text, "FAIL condition=distance")) << o.text;
}

TEST(RunPlan, SmokeRunSaysSoAndJudgesNothing) {
  const Outcome o =
      Execute(Header(1, Critical(1)) + Row("REF", "1e-12"), References(TrueValue() + 5.0),
              /*smoke=*/true);
  EXPECT_EQ(ExitCode(o.summary), 0) << o.text;
  EXPECT_TRUE(Has(o.text, "SMOKE RUN: reduced size, not a statistical result")) << o.text;
  EXPECT_TRUE(Has(o.text, "finite condition=not-judged")) << o.text;
  EXPECT_FALSE(Has(o.text, " pass ")) << o.text;
}

// A smoke run never touches the measured seed, so it cannot leak a look at the measured paths.
TEST(RunPlan, SmokeRunDoesNotUseTheMeasuredSeed) {
  const std::string text = Header(1, Critical(1)) + Row();
  const std::string full = Execute(text, References(TrueValue()), true).text;
  const std::string reseeded = [&] {
    std::string other = text;
    other.replace(other.find("measured_seed=9"), 15, "measured_seed=8");
    other.replace(other.find(",64,9,1,0,run"), 13, ",64,8,1,0,run");
    return Execute(other, References(TrueValue()), true).text;
  }();
  EXPECT_EQ(full, reseeded);
}

TEST(PlanTemplate, CoversEveryRequiredComparison) {
  const auto rows = PlanTemplate();
  std::set<std::string> ids;
  std::size_t run = 0;
  std::size_t awaiting = 0;
  std::size_t fd = 0;
  for (const auto& row : rows) {
    EXPECT_TRUE(ids.insert(row.id).second) << row.id;
    (row.status == RowStatus::kRun ? run : awaiting) += 1;
    if (FiniteDifferenceStep(row.reference)) ++fd;
  }
  EXPECT_EQ(run, 90U);
  EXPECT_EQ(awaiting, 57U);
  EXPECT_EQ(fd, 29U);  // Asian five, plus four on each of six discrete barriers.
  const auto has = [&](const std::string& id, const std::string& greek,
                       const std::string& reference) {
    for (const auto& row : rows) {
      if (row.id == id) return row.greek == greek && row.reference == reference;
    }
    return false;
  };
  EXPECT_TRUE(has("VA.delta", "delta", "O1"));
  EXPECT_TRUE(has("VB.rho", "rho", "O2"));
  EXPECT_TRUE(has("VC.theta", "theta", "O3"));
  EXPECT_TRUE(has("GA.value", "value", "O4"));
  EXPECT_TRUE(has("GA.vega", "vega", "GA.vega"));
  EXPECT_TRUE(has("AA50K100.value", "value", "O9"));
  EXPECT_TRUE(has("AA50K100.theta", "theta", "FD:0.0001"));
  EXPECT_TRUE(has("DB095.value", "value", "O5"));
  EXPECT_TRUE(has("DB095.delta", "delta", "O6"));
  EXPECT_TRUE(has("DB095.divrho", "divrho", "FD:0.0001"));
  EXPECT_TRUE(has("DBS.value", "value", "O8"));
  EXPECT_TRUE(has("CB095.value", "value", "O7"));
  EXPECT_TRUE(has("AA2.value", "value", "O11"));
}

TEST(PlanTemplate, EveryRunRowResolvesAgainstTheCommittedReferences) {
  std::ifstream file("references/oracle_values.csv", std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  const auto references = DecodeReferences(text.str());
  ASSERT_TRUE(references.has_value());
  for (const auto& row : PlanTemplate()) {
    if (row.status != RowStatus::kRun) continue;
    EXPECT_TRUE(DecodeInstrument(row.instrument).has_value()) << row.id;
    EXPECT_EQ(row.paths % 64, 0U) << row.id;
    if (FiniteDifferenceStep(row.reference)) continue;
    const Reference* reference = references->Find(row.reference);
    ASSERT_NE(reference, nullptr) << row.reference;
    EXPECT_EQ(reference->parameters, InstrumentParameters(row.instrument)) << row.id;
  }
}

TEST(BuildPlan, AppliesTheStatedRule) {
  std::vector<PlanTemplateRow> rows = {
      {"a.value", kInstrument, "value", "REF", 6400, RowStatus::kRun},
      {"a.vega", kInstrument, "vega", "FD:0.0001", 6400, RowStatus::kRun},
      {"z", "svi-surface:arithmetic", "d_a_d05", "phase3", 0, RowStatus::kAwaitingPhase3},
  };
  PilotFile pilot;
  pilot.config.seed = 7;
  pilot.config.batches = 64;
  pilot.rows = {{"a.value", 1600, 1.0, 1.0, 0.04, 0.0, 0.0},
                {"a.vega", 1600, 1.0, 1.0, 0.01, 3e-5, 1e-6}};
  const Plan plan = BuildPlan(rows, References(1.0), pilot, 9);
  ASSERT_EQ(plan.rows.size(), 3U);
  EXPECT_EQ(plan.header.declared_k, 3U);
  EXPECT_EQ(plan.header.critical, CriticalValue(0.01, 3, 64));
  // 1.65 * 0.04 * sqrt(1600 / 6400) = 0.033, up to three figures.
  EXPECT_NEAR(plan.rows[0].se_ceiling, 0.033, 1e-12);
  EXPECT_EQ(plan.rows[0].b, 0.0);
  EXPECT_EQ(plan.rows[0].seed, 9U);
  const double truncation = (3e-5 + plan.header.critical * 1e-6) / 3.0;
  EXPECT_GE(plan.rows[1].b, 2.0 * truncation);
  EXPECT_LE(plan.rows[1].b, 2.0 * truncation * 1.01);
  EXPECT_EQ(plan.rows[2].status, RowStatus::kAwaitingPhase3);
  const auto decoded = DecodePlan(EncodePlan(plan));
  ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
  EXPECT_EQ(*decoded, plan);
}

TEST(Pilot, FileRoundTrips) {
  PilotConfig config;
  config.seed = 7;
  const std::vector<PilotRow> rows = {{"a", 1600, 1.5, 1.25, 0.04, 3e-5, 1e-6}};
  PilotFile file;
  ASSERT_TRUE(DecodePilot(EncodePilot(rows, config), file));
  EXPECT_EQ(file.config.seed, 7U);
  ASSERT_EQ(file.rows.size(), 1U);
  EXPECT_EQ(file.rows[0].two_h_se, 1e-6);
  EXPECT_FALSE(DecodePilot("garbage,row\n1,2\n", file));
}

}  // namespace
}  // namespace aadp::oracle_runner
