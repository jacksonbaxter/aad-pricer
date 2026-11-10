// Executes references/comparison_plan.csv and judges every run row with the tolerance rule.
// Also holds the pilot and the step that turns its results into the plan, so the rule that sets
// each ceiling and b is code, committed before the plan it produces.
#ifndef AADP_TOOLS_RUN_ORACLES_RUNNER_HPP_
#define AADP_TOOLS_RUN_ORACLES_RUNNER_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "oracle/plan.hpp"
#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"

namespace aadp::oracle_runner {

// Stated in the plan header. See BuildPlan.
inline constexpr double kCeilingFactor = 1.65;
inline constexpr double kFdCeilingFactor = 3.0;
inline constexpr double kFdBFactor = 2.0;

struct RunOptions {
  bool smoke = false;
  std::uint32_t threads = 1;
  // A smoke run replaces N. The path count stays a multiple of B.
  std::uint64_t smoke_paths_per_batch = 64;
};

struct RunSummary {
  std::size_t rows = 0;
  std::size_t declared_k = 0;
  std::size_t run = 0;
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t skipped = 0;
  std::size_t awaiting = 0;
  // Run, never passed: a surface row whose ladder did not settle (NOTES.md, "Stage 8").
  std::size_t unresolved = 0;
  // Rows the frozen kernels cannot price, which the reference estimator lists and leaves to run 2.
  std::size_t run2_only = 0;
  double critical = 0.0;
  bool critical_matches_plan = false;

  [[nodiscard]] bool Clean() const {
    return failed == 0 && skipped == 0 && rows == declared_k && critical_matches_plan &&
           run == passed;
  }
};

// The rows of run 1: run rows on a GBM contract, the ones the frozen kernels and Dual price.
bool InRunOne(const PlanRow& row);

// The reference estimator: the frozen kernels of src/mc/ and the forward-mode Dual, on the rows of
// run 1. Every other row is printed as "<id> not-in-run-1" and counted as run2_only.
RunSummary RunPlan(const Plan& plan, const ReferenceTable& references, const RunOptions& options,
                   std::ostream& out);

// Same rows, but a smoke run judges nothing: it proves every row executes and stays finite.
inline int ExitCode(const RunSummary& summary) { return summary.Clean() ? 0 : 1; }

struct PlanTemplateRow {
  std::string id;
  std::string instrument;
  std::string greek;
  std::string reference;
  std::uint64_t paths = 0;
  RowStatus status = RowStatus::kRun;
  // One of the 57 rows of stage 8, piloted by the production estimator. Without stage 8's pilot
  // and ladder BuildPlan writes it as awaiting-phase3, as the plan of 5993b4a has it.
  bool stage8 = false;
};

// Every comparison, Phase 3 rows included. K is the size of this list.
std::vector<PlanTemplateRow> PlanTemplate();

// The reference field of a surface row before the ladder picks its step.
inline constexpr const char* kPhase3Placeholder = "phase3";

struct PilotRow {
  std::string id;
  std::uint64_t paths = 0;
  double estimate = 0.0;
  double reference = 0.0;
  double se = 0.0;
  double two_h_mean = 0.0;  // FD(2h) - FD(h), FD rows only.
  double two_h_se = 0.0;
};

struct PilotConfig {
  std::uint64_t seed = 0;
  std::uint64_t paths_divisor = 4;
  std::uint32_t batches = 64;
  std::uint32_t threads = 1;
};

std::vector<PilotRow> RunPilot(const std::vector<PlanTemplateRow>& rows,
                               const ReferenceTable& references, const PilotConfig& config,
                               std::ostream& log);
std::string EncodePilot(const std::vector<PilotRow>& rows, const PilotConfig& config);
struct PilotFile {
  PilotConfig config;
  std::vector<PilotRow> rows;
};
bool DecodePilot(std::string_view text, PilotFile& out);

// Stage 8's pilot file: the 57 rows in the same seven columns and directives, under a comment
// naming the production estimator. Knock-in rows: estimate the tape mean, reference the
// reference value, se the estimator's SE, the two-h columns 0. Surface rows: estimate the tape
// mean, reference the mean of D_{h*}, se the paired SE, and the two-h columns Delta_l*'s.
std::string EncodeStage8Pilot(const std::vector<PilotRow>& rows, const PilotConfig& config);

// Why a pilot may not run on this seed, or nothing: never on the plan's measured seed.
std::optional<std::string> RefusePilotSeed(std::uint64_t seed, const PlanHeader& header);

// One surface row's ladder outcome, as references/svi_fd_ladder.csv records it.
struct LadderChoice {
  std::string id;
  bool resolved = false;
  std::size_t level = 0;  // l*
  double h0 = 0.0;
  double h_star = 0.0;  // h_{l*+1}, or h_8 for an unresolved row
  double b = 0.0;
  std::string note;  // why a row is unresolved
};
// The choice records of a ladder file; nothing if the file is malformed.
std::optional<std::vector<LadderChoice>> DecodeLadderChoices(std::string_view text);

struct Stage8Inputs {
  PilotFile pilot;
  std::vector<LadderChoice> ladder;
};

// Up, so the printed ceiling is never below the rule's value. Zero stays zero.
double RoundUpToThreeFigures(double value);
// b of a finite-difference row: fd_b_factor * (|mean(FD(2h) - FD(h))| + critical * SE) / 3.
double LadderB(const BatchStatistics& two_h, double critical);

// Applies the stated rule:
//   se_ceiling = factor * SE_pilot * sqrt(N_pilot / N), ceiling_factor on reference rows and
//   fd_ceiling_factor on FD rows
//   b, reference rows = the reference's own b
//   b, FD rows = fd_b_factor * (|mean(FD(2h) - FD(h))| + t * SE of that mean) / 3
// Central differences have truncation c h^2, so FD(2h) - FD(h) = 3 c h^2 and a third of it is the
// truncation at h. t is the plan's critical value, so noise in the pilot widens b, not narrows it.
//
// Stage 8's rows (NOTES.md, "Stage 8"), from its own pilot and ladder: ceiling_factor on the 12
// knock-in rows, with the reference's b; fd_ceiling_factor on the 45 surface rows, with b by the
// FD formula from Delta_l* and the reference "FD:<h*>". A row whose ladder did not settle is
// unresolved, at "FD:<h_8>". Without stage8 those rows stay awaiting-phase3.
Plan BuildPlan(const std::vector<PlanTemplateRow>& rows, const ReferenceTable& references,
               const PilotFile& pilot, std::uint64_t measured_seed,
               const Stage8Inputs* stage8 = nullptr);

}  // namespace aadp::oracle_runner

#endif  // AADP_TOOLS_RUN_ORACLES_RUNNER_HPP_
