// Custom main for every benchmark binary. It writes one raw results file to stdout, in the
// format of results/results_file.hpp, and nothing else; run_bench.sh sends that to a file and
// results_stamp turns it into the file that gets committed.
//
// Warm-up happens twice. A harness stage runs the workload for --warmup-ms before anything is
// sized, so the caches and the frequency have settled. Then Google Benchmark's own warm-up runs
// through --benchmark_min_warmup_time. That has to be the flag: with a fixed iteration count
// Google Benchmark ignores the per-benchmark MinWarmUpTime. Samples from both stages are
// discarded, and the results header states the warm-up that ran.
//
// Google Benchmark calls a benchmark function once per warm-up attempt and once per repetition,
// with nothing to tell them apart. Warm-up always comes first, so the histogram takes the last
// `repetitions` calls and refuses if those are not all full length. Hardware counters are read
// per call and treated the same way, which also keeps out Google Benchmark's start-up: on macOS
// it spins for a second estimating a clock rate inside RunSpecifiedBenchmarks.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "latency.hpp"
#include "results/results_file.hpp"

namespace aadp::bench {

// Exit codes. A shell script tells a refusal from a crash by these.
inline constexpr int kExitOk = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitFpState = 3;
inline constexpr int kExitDirtyTree = 4;
inline constexpr int kExitHistogramRefused = 5;
inline constexpr int kExitPinFailed = 6;

struct Workload {
  std::string name;
  std::function<void(std::uint64_t units)> run;
};

struct HarnessOptions {
  std::optional<int> pin_cpu;
  bool reportable = false;
  std::filesystem::path source_root = ".";
  std::int64_t batches = 20'000;
  std::int64_t repetitions = 3;
  std::int64_t warmup_ms = 50;
  double gb_warmup_s = 0.05;
  std::optional<std::uint64_t> batch_units;  // Set by hand, the gate still applies
  std::vector<std::string> passthrough;      // argv[0] and the --benchmark_ flags
};

std::expected<HarnessOptions, std::string> parse_harness_options(int argc, char** argv);

// The body of a histogram section.
std::string render_histogram(std::uint64_t batch_units, const LatencyHistogram& histogram,
                             double timer_resolution_ns);

int harness_main(int argc, char** argv, std::vector<Workload> workloads);

}  // namespace aadp::bench
