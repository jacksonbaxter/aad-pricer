// The AAD tables (Phase 3): for each `aad_<workload>` section of a results file, the cost of every
// variant against the scalar primal and the accuracy it buys. The section is written by
// bench/aad_checks.cpp (RenderSection), whose header comment states each check's bound.
//
// Cost: time per run (Google Benchmark's mean real time of one batch, which is one run of all the
// workload's paths), paths per second, the multiple of the scalar primal's time, the slot for the
// multiple of the fastest (SIMD) primal that stage 5 fills, peak RSS of a fresh process running
// that variant alone and its ratio to the primal's, tape bytes per path, and the share of the
// tape's time that is the reverse sweep (tape - tape-record). Beside them: the fraction of paths
// with a nonzero payoff, since a tape skips nothing on those paths and a bump differences them all.
//
// Accuracy: each Greek's estimate and SE, the h study's bias at the bump's step with its SE (zero
// for the tape and the hand adjoint, which are pathwise derivatives up to round-off), and
// RMSE^2 x time, RMSE^2 = SE^2 + bias^2, the cost of a given accuracy.
//
// A workload whose in-run checks did not all pass is marked INVALID, and its multiples are not
// printed.
#ifndef AADP_RESULTS_AAD_TABLE_HPP_
#define AADP_RESULTS_AAD_TABLE_HPP_

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "results/results_file.hpp"

namespace aadp::results {

inline constexpr std::string_view kAadSectionPrefix = "aad_";

// The AAD tables of every file that has AAD sections, in file order; empty if none has. The
// caller has admitted the files (results/table.hpp).
std::expected<std::string, std::string> render_aad_tables(std::span<const ResultsFile> files);

}  // namespace aadp::results

#endif  // AADP_RESULTS_AAD_TABLE_HPP_
