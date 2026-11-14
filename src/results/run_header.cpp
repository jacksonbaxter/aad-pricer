#include "results/run_header.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "build_info.hpp"
#include "platform/affinity.hpp"
#include "platform/cpu_info.hpp"
#include "platform/fp_state.hpp"
#include "platform/host_label.hpp"
#include "platform/perf_counters.hpp"
#include "platform/simd_info.hpp"
#include "platform/timer.hpp"
#include "results/measure_line.hpp"
#include "results/provenance.hpp"
#include "results/results_file.hpp"

namespace aadp::results {
namespace {

std::string hex(std::uint64_t value) {
  std::array<char, 32> text{};
  std::snprintf(text.data(), text.size(), "0x%llx", static_cast<unsigned long long>(value));
  return text.data();
}

std::string utc_now() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm parts{};
  gmtime_r(&now, &parts);
  std::array<char, 32> text{};
  std::strftime(text.data(), text.size(), "%FT%TZ", &parts);
  return text.data();
}

bool apply_pin_cpu(std::string_view text, RunOptions& options) {
  int cpu = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), cpu);
  if (error != std::errc{} || end != text.data() + text.size() || cpu < 0) {
    return false;
  }
  options.pin_cpu = cpu;
  return true;
}

// Recorded, not enforced: nothing stops another process from running there.
bool apply_meant_idle_cpus(std::string_view text, RunOptions& options) {
  auto cpus = expand_cpu_list(text);
  if (!cpus) {
    return false;
  }
  options.meant_idle_cpus = std::move(*cpus);
  return true;
}

bool apply_source_root(std::string_view text, RunOptions& options) {
  options.source_root = std::string(text);
  return true;
}

struct RunFlag {
  std::string_view flag;
  bool (*apply)(std::string_view, RunOptions&);
};

constexpr auto kRunFlags = std::to_array<RunFlag>({
    {.flag = "--pin-cpu=", .apply = apply_pin_cpu},
    {.flag = "--meant-idle-cpus=", .apply = apply_meant_idle_cpus},
    {.flag = "--source-root=", .apply = apply_source_root},
});

}  // namespace

std::string header_number(double value, const char* format) {
  std::array<char, 64> text{};
  std::snprintf(text.data(), text.size(), format, value);
  return text.data();
}

std::expected<bool, std::string> apply_run_flag(std::string_view arg, RunOptions& options) {
  if (arg == "--reportable") {
    options.reportable = true;
    return true;
  }
  const auto* flag = std::ranges::find_if(
      kRunFlags, [arg](const RunFlag& candidate) { return arg.starts_with(candidate.flag); });
  if (flag == kRunFlags.end()) {
    return false;
  }
  if (!flag->apply(arg.substr(flag->flag.size()), options)) {
    return std::unexpected("bad option " + std::string(arg));
  }
  return true;
}

std::expected<std::vector<std::string>, std::string> take_run_flags(int argc, char* const* argv,
                                                                    RunOptions& options) {
  std::vector<std::string> rest;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto taken = apply_run_flag(arg, options);
    if (!taken) {
      return std::unexpected(taken.error());
    }
    if (!*taken) {
      rest.emplace_back(arg);
    }
  }
  return rest;
}

std::expected<RunStart, RunRefusal> start_run(const RunOptions& options) {
  const auto fp_before = platform::check_fp_state();
  if (!fp_before) {
    return std::unexpected(RunRefusal{.exit_code = kRunExitFpState,
                                      .message = "refusing to run: " + fp_before.error()});
  }
  RunStart start;
  start.options = options;
  if (options.pin_cpu) {
    auto pinned = platform::pin_to_cpu(*options.pin_cpu);
    if (!pinned) {
      return std::unexpected(RunRefusal{.exit_code = kRunExitPinFailed, .message = pinned.error()});
    }
    start.pin_method = "--pin-cpu, sched_setaffinity, read back";
  }

  start.provenance = read_provenance(options.source_root);
  if (options.reportable && start.provenance.tree_state != "clean" &&
      start.provenance.tree_state != "exported") {
    return std::unexpected(
        RunRefusal{.exit_code = kRunExitDirtyTree,
                   .message = "reportable run refused: tree is " + start.provenance.tree_state});
  }

  start.affinity = platform::read_affinity();
  if (start.affinity.pinned && !options.pin_cpu) {
    start.pin_method = "inherited mask, read back";
  }

  start.started_at = utc_now();
  start.cpu = platform::read_cpu_info();
  start.host_label = std::string(platform::classify_host(start.cpu.os_name, start.cpu.cpu_model));
  start.timer = platform::calibrate_timer();
  if (const std::string problem = platform::timer_problem(start.timer); !problem.empty()) {
    return std::unexpected(
        RunRefusal{.exit_code = kRunExitFailure, .message = "timer unusable: " + problem});
  }
  return start;
}

std::vector<HeaderField> run_header_fields(const RunStart& start, const RunShape& shape,
                                           const platform::FpState& fp_after,
                                           const platform::AffinityState& affinity_after) {
  std::vector<HeaderField> header;
  const auto add = [&header](std::string_view key, std::string value) {
    header.push_back({.key = std::string(key), .value = std::move(value)});
  };
  const platform::CpuInfo& cpu = start.cpu;
  const platform::TimerReport& timer = start.timer;
  // "none" unless the writer says otherwise. Only a run of the project's own code may claim
  // "result"; the self-tests claim nothing.
  add("claim", shape.claim);
  add("host_label", start.host_label);
  add("cpu_model", cpu.cpu_model);
  add("logical_cpus", std::to_string(cpu.logical_cpus));
  add("arch", cpu.arch);
  add("kernel", cpu.kernel);
  add("os", cpu.os);
  add("compiler_id", std::string(build_info::compiler_id));
  add("compiler_version", std::string(build_info::compiler_version));
  add("cxx_standard", std::to_string(build_info::cxx_standard));
  add("cxx_flags", std::string(build_info::cxx_flags));
  add("build_type", std::string(build_info::build_type));
  add("simd_cmake_target", std::string(build_info::simd_target));
  add("simd_target", platform::simd_target_name());
  add("simd_double_lanes", std::to_string(platform::simd_double_lanes()));
  add("pinning_state", std::string(platform::pinning_state_text(affinity_after)));
  add("pinned_cpus",
      affinity_after.pinned ? platform::format_cpu_list(affinity_after.cpus) : "none");
  add(kMeantIdleCpusKey,
      start.options.meant_idle_cpus.empty()
          ? "none"
          : platform::format_cpu_list(start.options.meant_idle_cpus) + std::string(kNotEnforced));
  add("pin_method", start.pin_method);
  add("observed_cpu",
      affinity_after.current ? std::to_string(*affinity_after.current) : "unavailable");
  add("qos_class", affinity_after.qos_class);
  add("process_nice", std::to_string(platform::process_nice()));
  add("fp_register", std::string(fp_after.register_name));
  add("fp_state", hex(fp_after.raw));
  add("fp_ftz", fp_after.ftz ? "set" : "clear");
  add("fp_daz", fp_after.daz ? "set" : "clear");
  add("timer_source", timer.tick_source);
  add("timer_calibrated_hz", header_number(timer.ticks_per_second, "%.1f"));
  add("timer_calibration_spread", header_number(timer.calibration_spread, "%.6f"));
  add("timer_granularity_ticks", std::to_string(timer.tick_reads.granularity));
  add("timer_min_delta_ticks", std::to_string(timer.tick_reads.min_nonzero_delta));
  add("timer_resolution_ns", header_number(timer.tick_resolution_ns));
  add("timer_read_cost_ns", header_number(timer.tick_read_cost_ns));
  add("timer_note", timer.tick_note);
  add("clock_source", timer.clock_source);
  add("clock_getres_ns", header_number(timer.clock_getres_ns, "%.1f"));
  add("clock_resolution_ns", header_number(timer.clock_resolution_ns, "%.1f"));
  add("repetitions", std::to_string(shape.repetitions));
  add("batches_per_repetition", std::to_string(shape.batches_per_repetition));
  add("warmup", shape.warmup);
  add("commit", start.provenance.commit);
  add("commit_source", start.provenance.source);
  add("tree_state", start.provenance.tree_state);
  add("run_started_at", start.started_at);
  return header;
}

void add_run_late_fields(std::vector<HeaderField>& header, const platform::FrequencyInfo& freq,
                         const platform::PerfCounterReport& perf, std::uint64_t peak_rss) {
  const auto add = [&header](std::string_view key, std::string value) {
    header.push_back({.key = std::string(key), .value = std::move(value)});
  };
  add("smt_siblings", freq.smt_siblings);
  add("governor", freq.governor);
  add("cpufreq_driver", freq.driver);
  add("frequency_khz", freq.frequency_khz);
  add("max_frequency_khz", freq.max_frequency_khz);
  add("turbo", freq.turbo);
  add("perf_counters_source", perf.source);
  add("perf_counters_scope", perf.scope);
  add("perf_cycles", perf.cycles ? std::to_string(*perf.cycles) : "missing");
  add("perf_instructions", perf.instructions ? std::to_string(*perf.instructions) : "missing");
  add("perf_counters_note", platform::describe_counters(perf));
  add("peak_rss_bytes", std::to_string(peak_rss));
}

}  // namespace aadp::results
