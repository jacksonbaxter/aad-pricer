// Executes references/comparison_plan.csv and judges every run row with the tolerance rule.
// Also holds the pilot and the step that turns its results into the plan, so the rule that sets
// each ceiling and b is code, committed before the plan it produces.
#ifndef AADP_TOOLS_RUN_ORACLES_RUNNER_HPP_
#define AADP_TOOLS_RUN_ORACLES_RUNNER_HPP_

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "oracle/plan.hpp"
#include "oracle/references.hpp"

namespace aadp::oracle_runner {

// Stated in the plan header. See BuildPlan.
inline constexpr double kCeilingFactor = 1.65;
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
  double critical = 0.0;
  bool critical_matches_plan = false;

  bool Clean() const {
    return failed == 0 && skipped == 0 && rows == declared_k && critical_matches_plan &&
           run == passed;
  }
};

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
};

// Every comparison, Phase 3 rows included. K is the size of this list.
std::vector<PlanTemplateRow> PlanTemplate();

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

// Applies the stated rule:
//   se_ceiling = ceiling_factor * SE_pilot * sqrt(N_pilot / N)
//   b, reference rows = the reference's own b
//   b, FD rows = fd_b_factor * (|mean(FD(2h) - FD(h))| + t * SE of that mean) / 3
// Central differences have truncation c h^2, so FD(2h) - FD(h) = 3 c h^2 and a third of it is the
// truncation at h. t is the plan's critical value, so noise in the pilot widens b, not narrows it.
Plan BuildPlan(const std::vector<PlanTemplateRow>& rows, const ReferenceTable& references,
               const PilotFile& pilot, std::uint64_t measured_seed);

}  // namespace aadp::oracle_runner

#endif  // AADP_TOOLS_RUN_ORACLES_RUNNER_HPP_
