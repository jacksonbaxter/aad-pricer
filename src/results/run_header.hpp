// The run header every results file carries, benchmark or report, written by one piece of code:
// the bench harness (bench/harness.hpp) and every report emitter (results/report_run.hpp) call
// these, in this order, so a report says what a benchmark says about where and how it ran.
//
//   start_run            the run flags, FP state, pin, provenance, CPU, host label and timer
//   run_header_fields    claim .. run_started_at, after the work (the order of a results file)
//   add_run_late_fields  smt_siblings .. peak_rss_bytes, read last
//
// Between the second and the third the caller adds its own header fields. The keys are those of
// results/results_file.hpp; the stamp checks every one.
#ifndef AADP_RESULTS_RUN_HEADER_HPP_
#define AADP_RESULTS_RUN_HEADER_HPP_

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/affinity.hpp"
#include "platform/cpu_info.hpp"
#include "platform/fp_state.hpp"
#include "platform/perf_counters.hpp"
#include "platform/timer.hpp"
#include "results/provenance.hpp"
#include "results/results_file.hpp"

namespace aadp::results {

// Exit codes of a refused run, the same for a benchmark binary and a report emitter, so that
// scripts/run_bench.sh and its tests read them alike.
inline constexpr int kRunExitFailure = 1;
inline constexpr int kRunExitUsage = 2;
inline constexpr int kRunExitFpState = 3;
inline constexpr int kRunExitDirtyTree = 4;
inline constexpr int kRunExitPinFailed = 6;

// The flags every writer of a results file takes. scripts/run_bench.sh passes --source-root=.,
// --reportable when the run is, and --meant-idle-cpus=5 on linux-zen1.
struct RunOptions {
  std::optional<int> pin_cpu;
  std::vector<int> meant_idle_cpus;  // --meant-idle-cpus=5: written to the header, not enforced
  bool reportable = false;
  std::filesystem::path source_root = ".";
};

// True if `arg` is one of the run flags, which it then applies; false if it is not one; an error
// for a run flag with a bad value.
std::expected<bool, std::string> apply_run_flag(std::string_view arg, RunOptions& options);

// The flags after argv[0]: the run flags applied to `options`, the rest returned in order.
std::expected<std::vector<std::string>, std::string> take_run_flags(int argc, char* const* argv,
                                                                    RunOptions& options);

struct RunStart {
  RunOptions options;
  std::string pin_method = "none";
  Provenance provenance;
  platform::AffinityState affinity;
  std::string started_at;
  platform::CpuInfo cpu;
  std::string host_label;
  platform::TimerReport timer;
};

struct RunRefusal {
  int exit_code = kRunExitFailure;
  std::string message;  // Without the tool's name
};

// Everything a run checks and reads before its work, in the harness's order: the FP state, the
// pin, the provenance (a reportable run from a tree that is neither clean nor exported is refused),
// the affinity, the start time, the CPU, the host label and the timer.
std::expected<RunStart, RunRefusal> start_run(const RunOptions& options);

// What the run's header says about its repetitions, for a report as for a benchmark.
struct RunShape {
  std::string claim = "none";
  std::int64_t repetitions = 1;
  std::int64_t batches_per_repetition = 1;
  std::string warmup = "none";
};

// claim .. run_started_at, with the FP state and the affinity read after the work.
std::vector<HeaderField> run_header_fields(const RunStart& start, const RunShape& shape,
                                           const platform::FpState& fp_after,
                                           const platform::AffinityState& affinity_after);

// smt_siblings .. peak_rss_bytes, the frequency read on `report_cpu`.
void add_run_late_fields(std::vector<HeaderField>& header, const platform::FrequencyInfo& freq,
                         const platform::PerfCounterReport& perf, std::uint64_t peak_rss);

// %.3f and the like, as the header writes its numbers.
std::string header_number(double value, const char* format = "%.3f");

}  // namespace aadp::results

#endif  // AADP_RESULTS_RUN_HEADER_HPP_
