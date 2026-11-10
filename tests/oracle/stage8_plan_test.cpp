// Stage 8's plan rules (NOTES.md, "Stage 8"): emit-plan keeps run 1's rows, directives and comment
// lines byte for byte; the knock-in rows take 1.65 and their reference's b; the surface rows take
// 3, b by the FD formula from the ladder, and FD:<h*>; an unresolved row round-trips, runs, prints
// UNRESOLVED and fails the run; the reference estimator's run-1 lines do not change; and the pilot
// refuses the measured seed.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "oracle/decode_util.hpp"
#include "oracle/plan.hpp"
#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"
#include "production.hpp"
#include "runner.hpp"
#include "svi/chain_surface.hpp"
#include "svi/surface.hpp"
#include "svi/synthetic_surface.hpp"

namespace aadp::oracle_runner {
namespace {

std::string ReadFile(const std::string& path) {
  const std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

ReferenceTable CommittedReferences() {
  auto references = DecodeReferences(ReadFile("references/oracle_values.csv"));
  EXPECT_TRUE(references.has_value());
  return *std::move(references);
}

PilotFile CommittedPilot() {
  PilotFile pilot;
  EXPECT_TRUE(DecodePilot(ReadFile("references/pilot_results.csv"), pilot));
  return pilot;
}

constexpr std::uint64_t kMeasuredSeed = 9;  // Made up; the plan's own is read from its header.
constexpr std::uint64_t kStage8PilotPaths = 1U << 20;
const std::string kUnresolvedId = "P3.CB.d12.m";

// Made-up stage 8 inputs: every row a distinct SE, every surface row a distinct level, and one
// row unresolved.
Stage8Inputs MadeUpStage8(double critical) {
  Stage8Inputs out;
  out.pilot.config.seed = 7;
  out.pilot.config.batches = 64;
  out.pilot.config.paths_divisor = 4;
  std::size_t n = 0;
  for (const PlanTemplateRow& row : PlanTemplate()) {
    if (!row.stage8) continue;
    ++n;
    PilotRow p{.id = row.id,
               .paths = kStage8PilotPaths,
               .estimate = 1.0,
               .reference = 1.0,
               .se = 1e-3 * static_cast<double>(n),
               .two_h_mean = 0.0,
               .two_h_se = 0.0};
    if (row.instrument.starts_with("svi-surface:")) {
      p.two_h_mean = 1e-4 * static_cast<double>(n);
      p.two_h_se = 1e-5 * static_cast<double>(n);
      LadderChoice c;
      c.id = row.id;
      c.resolved = row.id != kUnresolvedId;
      c.h0 = 1e-4;
      c.level = n % 6;
      c.h_star = std::ldexp(c.h0, -static_cast<int>(c.resolved ? c.level + 1 : 8));
      if (c.resolved) {
        c.b = LadderB({.mean = p.two_h_mean, .standard_error = p.two_h_se}, critical);
      } else {
        c.note = "no level";
        p.two_h_mean = 0.0;
        p.two_h_se = 0.0;
      }
      out.ladder.push_back(c);
    }
    out.pilot.rows.push_back(p);
  }
  return out;
}

std::map<std::string, std::string> RowLines(const std::string& text) {
  std::map<std::string, std::string> out;
  for (const std::string_view line : decode::Lines(text)) {
    if (line.empty() || line.front() == '#' || line.starts_with("id,")) continue;
    out.emplace(std::string(line.substr(0, line.find(','))), std::string(line));
  }
  return out;
}

std::set<std::string> RunOneIds() {
  std::set<std::string> ids;
  for (const PlanTemplateRow& row : PlanTemplate()) {
    if (!row.stage8) ids.insert(row.id);
  }
  return ids;
}

TEST(Stage8Plan, RunOneRowsDirectivesAndCommentsKeepTheirBytes) {
  const ReferenceTable references = CommittedReferences();
  const PilotFile pilot = CommittedPilot();
  const auto committed = DecodePlan(ReadFile("references/comparison_plan.csv"));
  ASSERT_TRUE(committed.has_value());
  const std::uint64_t seed = committed->header.measured_seed;
  const std::string before = EncodePlan(BuildPlan(PlanTemplate(), references, pilot, seed));
  const Stage8Inputs stage8 = MadeUpStage8(committed->header.critical);
  const std::string after = EncodePlan(BuildPlan(PlanTemplate(), references, pilot, seed, &stage8));

  const auto directives = [](const std::string& text) {
    std::vector<std::string> out;
    for (const std::string_view line : decode::Lines(text)) {
      if (line.starts_with("#!")) out.emplace_back(line);
    }
    return out;
  };
  EXPECT_EQ(directives(before), directives(after));
  const auto comments = [](const std::string& text) {
    std::vector<std::string> out;
    for (const std::string_view line : decode::Lines(text)) {
      if (line.starts_with("# ")) out.emplace_back(line);
    }
    return out;
  };
  const std::vector<std::string> old_comments = comments(before);
  const std::vector<std::string> new_comments = comments(after);
  ASSERT_EQ(new_comments.size(), old_comments.size() + 2);
  for (std::size_t i = 0; i < old_comments.size(); ++i) EXPECT_EQ(new_comments[i], old_comments[i]);

  // Run 1's rows: the same bytes before stage 8, after it, and in the committed plan.
  const auto old_rows = RowLines(before);
  const auto new_rows = RowLines(after);
  const auto committed_rows = RowLines(ReadFile("references/comparison_plan.csv"));
  std::size_t checked = 0;
  for (const std::string& id : RunOneIds()) {
    ASSERT_TRUE(old_rows.contains(id)) << id;
    EXPECT_EQ(new_rows.at(id), old_rows.at(id)) << id;
    EXPECT_EQ(committed_rows.at(id), old_rows.at(id)) << id;
    ++checked;
  }
  EXPECT_EQ(checked, 90U);
  EXPECT_EQ(directives(ReadFile("references/comparison_plan.csv")), directives(before));
}

TEST(Stage8Plan, KnockInsTakeTheirFactorAndSurfacesTheirsWithTheLaddersStep) {
  const ReferenceTable references = CommittedReferences();
  const PilotFile pilot = CommittedPilot();
  const double critical = CriticalValue(0.01, PlanTemplate().size(), 64);
  const Stage8Inputs stage8 = MadeUpStage8(critical);
  const Plan plan = BuildPlan(PlanTemplate(), references, pilot, kMeasuredSeed, &stage8);
  ASSERT_EQ(plan.rows.size(), 147U);
  EXPECT_EQ(plan.header.declared_k, 147U);
  std::map<std::string, const PilotRow*> pilots;
  for (const PilotRow& p : stage8.pilot.rows) pilots[p.id] = &p;
  std::map<std::string, const LadderChoice*> choices;
  for (const LadderChoice& c : stage8.ladder) choices[c.id] = &c;
  std::size_t knock_ins = 0;
  std::size_t surfaces = 0;
  std::size_t unresolved = 0;
  for (const PlanRow& row : plan.rows) {
    const auto instrument = ResolveInstrument(row.instrument);
    if (!instrument) FAIL() << row.id;
    if (instrument->kind == InstrumentKind::kGbm) continue;
    const PilotRow& p = *pilots.at(row.id);
    const double scale = std::sqrt(static_cast<double>(p.paths) / static_cast<double>(row.paths));
    EXPECT_EQ(row.paths, 1U << 22) << row.id;
    EXPECT_EQ(row.seed, kMeasuredSeed) << row.id;
    if (instrument->kind == InstrumentKind::kKnockIn) {
      ++knock_ins;
      EXPECT_EQ(row.status, RowStatus::kRun) << row.id;
      EXPECT_EQ(row.reference, row.id) << row.id;
      EXPECT_EQ(row.se_ceiling, RoundUpToThreeFigures(1.65 * p.se * scale)) << row.id;
      EXPECT_EQ(row.b, references.Find(row.id)->b) << row.id;
      continue;
    }
    ++surfaces;
    const LadderChoice& c = *choices.at(row.id);
    EXPECT_EQ(row.reference, "FD:" + decode::FormatDouble(c.h_star)) << row.id;
    EXPECT_NE(row.reference, "FD:" + decode::FormatDouble(c.h0)) << row.id;
    if (!c.resolved) {
      ++unresolved;
      EXPECT_EQ(row.status, RowStatus::kUnresolved) << row.id;
      continue;
    }
    EXPECT_EQ(row.status, RowStatus::kRun) << row.id;
    EXPECT_EQ(row.se_ceiling, RoundUpToThreeFigures(3.0 * p.se * scale)) << row.id;
    EXPECT_EQ(row.b, c.b) << row.id;
  }
  EXPECT_EQ(knock_ins, 12U);
  EXPECT_EQ(surfaces, 45U);
  EXPECT_EQ(unresolved, 1U);
  const auto decoded = DecodePlan(EncodePlan(plan));
  ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
  EXPECT_EQ(*decoded, plan);
}

// Surface A's parameters with forwards scaled to the captured chain's level, so stage 7a's
// contracts (K = 22,800, H = 21,500) sit where they do on the chain.
svi::ChainSurface ScaledSurfaceA() {
  const svi::testing::Surface a = svi::testing::SurfaceA();
  svi::ChainSurface out;
  for (std::size_t j = 0; j < a.terms.size(); ++j) {
    svi::SliceTerms t = a.terms[j];
    t.forward *= 230.0;
    out.terms.push_back(t);
    out.params.push_back(a.params[j]);
  }
  return out;
}

TEST(Stage8Plan, AnUnresolvedRowRoundTripsRunsAndFailsTheRun) {
  const std::string text =
      "#! K=1\n#! alpha_F=0.01\n#! B=64\n#! critical=" +
      decode::FormatDouble(CriticalValue(0.01, 1, 64)) +
      "\n#! ceiling_factor=1.65\n#! fd_ceiling_factor=3\n#! fd_b_factor=2\n#! pilot_seed=7\n"
      "#! measured_seed=9\n"
      "id,instrument,greek,reference,N,B,seed,se_ceiling,b,status\n"
      "u,svi-surface:discrete-barrier,d_m_d12,FD:1e-08,4096,64,9,-,-,unresolved\n";
  const auto plan = DecodePlan(text);
  ASSERT_TRUE(plan.has_value()) << plan.error().message;
  ASSERT_EQ(plan->rows.size(), 1U);
  EXPECT_EQ(plan->rows[0].status, RowStatus::kUnresolved);
  const std::string encoded = EncodePlan(*plan);
  EXPECT_TRUE(encoded.ends_with(
      "u,svi-surface:discrete-barrier,d_m_d12,FD:1e-08,4096,64,9,-,-,unresolved\n"))
      << encoded;
  const auto again = DecodePlan(encoded);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, *plan);

  const svi::ChainSurface surface = ScaledSurfaceA();
  std::ostringstream out;
  const RunSummary summary =
      RunPlanProduction(*plan, ReferenceTable{}, &surface, {.threads = 2}, out);
  EXPECT_TRUE(out.str().contains(" UNRESOLVED condition=unresolved")) << out.str();
  EXPECT_EQ(summary.unresolved, 1U);
  EXPECT_EQ(summary.passed, 0U);
  EXPECT_EQ(summary.skipped, 0U);
  EXPECT_NE(ExitCode(summary), 0);

  // A plan row may not fix a ceiling or b for an unresolved row.
  std::string fixed = text;
  fixed.replace(fixed.find(",-,-,unresolved"), 15, ",1,0,unresolved");
  EXPECT_FALSE(DecodePlan(fixed).has_value());
}

TEST(Stage8Plan, TheProductionEstimatorRefusesAwaitingRows) {
  const std::string text =
      "#! K=1\n#! alpha_F=0.01\n#! B=64\n#! critical=" +
      decode::FormatDouble(CriticalValue(0.01, 1, 64)) +
      "\n#! ceiling_factor=1.65\n#! fd_ceiling_factor=3\n#! fd_b_factor=2\n#! pilot_seed=7\n"
      "#! measured_seed=9\n"
      "id,instrument,greek,reference,N,B,seed,se_ceiling,b,status\n"
      "z,svi-surface:arithmetic,d_a_d05,phase3,-,64,-,-,-,awaiting-phase3\n";
  const auto plan = DecodePlan(text);
  ASSERT_TRUE(plan.has_value());
  const svi::ChainSurface surface = ScaledSurfaceA();
  std::ostringstream out;
  const RunSummary summary =
      RunPlanProduction(*plan, ReferenceTable{}, &surface, {.threads = 2}, out);
  EXPECT_NE(ExitCode(summary), 0);
  EXPECT_TRUE(out.str().contains("SKIPPED: awaiting-phase3")) << out.str();
}

TEST(Stage8Plan, ReferenceModeKeepsRunOnesLines) {
  const ReferenceTable references = CommittedReferences();
  const PilotFile pilot = CommittedPilot();
  const auto committed = DecodePlan(ReadFile("references/comparison_plan.csv"));
  ASSERT_TRUE(committed.has_value());
  const std::uint64_t seed = committed->header.measured_seed;
  const Plan before = BuildPlan(PlanTemplate(), references, pilot, seed);
  const Stage8Inputs stage8 = MadeUpStage8(committed->header.critical);
  const Plan after = BuildPlan(PlanTemplate(), references, pilot, seed, &stage8);
  RunOptions options;
  options.smoke = true;
  options.threads = 4;
  std::ostringstream old_out;
  std::ostringstream new_out;
  const RunSummary old_summary = RunPlan(before, references, options, old_out);
  const RunSummary new_summary = RunPlan(after, references, options, new_out);
  EXPECT_EQ(ExitCode(new_summary), 0) << new_out.str();
  EXPECT_EQ(new_summary.run, 90U);
  EXPECT_EQ(new_summary.run2_only, 57U);
  EXPECT_EQ(old_summary.awaiting, 57U);
  const std::set<std::string> ids = RunOneIds();
  const auto run_one = [&](const std::string& text) {
    std::vector<std::string> out;
    for (const std::string_view line : decode::Lines(text)) {
      const std::string id(line.substr(0, line.find(' ')));
      if (ids.contains(id)) out.emplace_back(line);
    }
    return out;
  };
  const std::vector<std::string> old_lines = run_one(old_out.str());
  EXPECT_EQ(old_lines.size(), 90U);
  EXPECT_EQ(run_one(new_out.str()), old_lines);
  std::size_t listed = 0;
  for (const std::string_view line : decode::Lines(new_out.str())) {
    if (line.ends_with(" not-in-run-1")) ++listed;
  }
  EXPECT_EQ(listed, 57U);
}

TEST(Stage8Plan, ThePilotRefusesTheMeasuredSeed) {
  const auto committed = DecodePlan(ReadFile("references/comparison_plan.csv"));
  ASSERT_TRUE(committed.has_value());
  const PlanHeader& header = committed->header;
  EXPECT_TRUE(RefusePilotSeed(header.measured_seed, header).has_value());
  EXPECT_TRUE(RefusePilotSeed(0, header).has_value());
  EXPECT_FALSE(RefusePilotSeed(header.pilot_seed, header).has_value());
}

}  // namespace
}  // namespace aadp::oracle_runner
