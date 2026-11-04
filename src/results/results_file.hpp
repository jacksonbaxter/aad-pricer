// The results file: a magic line, `key: value` header lines, a blank line, then named sections.
//
//   aadp-results 1
//   claim: none
//   host_label: mac-m4max
//
//   @section benchmark_json
//   { ... }
//   @section histogram_harness_selftest
//   p50 4167
//
// The binary writes this format and results_stamp adds the keys it owns. Keeping one format
// means one reader and one fuzz target.
#ifndef AADP_RESULTS_RESULTS_FILE_HPP_
#define AADP_RESULTS_RESULTS_FILE_HPP_

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aadp::results {

inline constexpr std::string_view kResultsMagic = "aadp-results 1";
inline constexpr std::string_view kBenchmarkSection = "benchmark_json";
inline constexpr std::string_view kHistogramPrefix = "histogram_";

struct HeaderField {
  std::string key;
  std::string value;
  bool operator==(const HeaderField&) const = default;
};

struct ResultsSection {
  std::string name;
  std::string body;  // Empty, or ending in a newline
  bool operator==(const ResultsSection&) const = default;
};

struct ResultsFile {
  std::vector<HeaderField> header;
  std::vector<ResultsSection> sections;

  [[nodiscard]] std::optional<std::string_view> value(std::string_view key) const;
  // For a key the caller has already proved present, as validate_stamped does. An absent key
  // reads as empty, so a slip fails the caller's next comparison rather than reading nothing.
  [[nodiscard]] std::string_view required(std::string_view key) const {
    return value(key).value_or(std::string_view{});
  }
  [[nodiscard]] const ResultsSection* section(std::string_view name) const;
  bool operator==(const ResultsFile&) const = default;
};

// Every key the binary must write.
inline constexpr auto kRunHeaderKeys = std::to_array<std::string_view>({
    "claim",
    "host_label",
    "cpu_model",
    "logical_cpus",
    "arch",
    "kernel",
    "os",
    "compiler_id",
    "compiler_version",
    "cxx_standard",
    "cxx_flags",
    "build_type",
    "simd_cmake_target",
    "simd_target",
    "simd_double_lanes",
    "pinning_state",
    "pinned_cpus",
    "pin_method",
    "observed_cpu",
    "smt_siblings",
    "qos_class",
    "process_nice",
    "governor",
    "cpufreq_driver",
    "frequency_khz",
    "max_frequency_khz",
    "turbo",
    "fp_register",
    "fp_state",
    "fp_ftz",
    "fp_daz",
    "timer_source",
    "timer_calibrated_hz",
    "timer_calibration_spread",
    "timer_granularity_ticks",
    "timer_min_delta_ticks",
    "timer_resolution_ns",
    "timer_read_cost_ns",
    "timer_note",
    "clock_source",
    "clock_getres_ns",
    "clock_resolution_ns",
    "repetitions",
    "batches_per_repetition",
    "warmup",
    "commit",
    "commit_source",
    "tree_state",
    "peak_rss_bytes",
    "run_started_at",
});

inline constexpr auto kPerfHeaderKeys = std::to_array<std::string_view>({
    "perf_counters_source",
    "perf_counters_scope",
    "perf_cycles",
    "perf_instructions",
    "perf_counters_note",
});

// The CPUs a run means to leave idle, such as cpu5, the SMT sibling of linux-zen1's cpu4: a
// list and "(not enforced)", or "none". The harness writes it since Phase 3 stage 3b and the stamp
// requires it on linux-zen1. It is not in kRunHeaderKeys, so files stamped before it still read.
inline constexpr std::string_view kMeantIdleCpusKey = "meant_idle_cpus";
inline constexpr std::string_view kNotEnforced = " (not enforced)";

// pinned_cpus holds at most this many CPUs: one, or two for a threaded row (taskset -c 4,6). The
// harness calls the same limit platform::kMaxPinnedCpus, and checks the two agree.
inline constexpr std::size_t kMaxPinnedCpus = 2;

// Keys only the stamp tool may write. A binary's output that has them is refused.
inline constexpr auto kStampHeaderKeys = std::to_array<std::string_view>({
    "reportable",
    "reportable_reason",
    "measure_start",
    "measure_end",
});

std::expected<ResultsFile, std::string> parse_results_file(std::string_view text);

// Refuses anything the reader would not read back as the same file.
std::expected<std::string, std::string> write_results_file(const ResultsFile& file);

// Those of `keys` that the file does not have, or has with an empty value.
std::vector<std::string_view> missing_keys(const ResultsFile& file,
                                           std::span<const std::string_view> keys);

// All run keys, perf keys and stamp keys.
std::vector<std::string_view> missing_stamped_keys(const ResultsFile& file);

bool is_commit_hash(std::string_view text);

// The rules any stamped file obeys, checked again by every reader so that a file edited by hand
// cannot enter a table.
std::expected<void, std::string> validate_stamped(const ResultsFile& file);

struct HistogramValue {
  std::string key;
  std::string value;
};

std::vector<HistogramValue> parse_histogram_body(std::string_view body);

}  // namespace aadp::results

#endif  // AADP_RESULTS_RESULTS_FILE_HPP_
