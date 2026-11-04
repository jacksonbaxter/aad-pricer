#include "harness.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "build_info.hpp"
#include "latency.hpp"
#include "platform/affinity.hpp"
#include "platform/cpu_info.hpp"
#include "platform/fp_state.hpp"
#include "platform/host_label.hpp"
#include "platform/memory_info.hpp"
#include "platform/perf_counters.hpp"
#include "platform/simd_info.hpp"
#include "platform/timer.hpp"
#include "results/measure_line.hpp"
#include "results/provenance.hpp"
#include "results/results_file.hpp"

namespace aadp::bench {

static_assert(platform::kMaxPinnedCpus == results::kMaxPinnedCpus,
              "the harness must not call pinned what the stamp refuses");

namespace {

using platform::read_ticks;

std::string number(double value, const char* format = "%.3f") {
  std::array<char, 64> text{};
  std::snprintf(text.data(), text.size(), format, value);
  return text.data();
}

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

template <typename T>
bool parse_integer(std::string_view text, T& out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc{} && end == text.data() + text.size();
}

struct WorkloadRun {
  Workload workload;
  std::uint64_t units = 0;
  double seconds_per_tick = 0;
  std::vector<std::vector<std::uint64_t>> calls;  // Ticks per batch, one vector per call
  platform::PerfCounterSession counters;
  std::vector<platform::PerfCounterReport> call_counters;
};

// Google Benchmark's own loop goes around this, with manual time: the time it reports is the
// same batch span the histogram sees, and excludes the push_back.
void run_batches(benchmark::State& state, WorkloadRun& run) {
  std::vector<std::uint64_t>& ticks = run.calls.emplace_back();
  ticks.reserve(static_cast<std::size_t>(state.max_iterations));
  run.counters.start();
  for ([[maybe_unused]] const auto _ : state) {
    const std::uint64_t start = read_ticks();
    run.workload.run(run.units);
    const std::uint64_t span = read_ticks() - start;
    ticks.push_back(span);
    state.SetIterationTime(static_cast<double>(span) * run.seconds_per_tick);
  }
  run.call_counters.push_back(run.counters.stop());
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(run.units));
}

std::vector<results::HeaderField> build_header(
    const std::string& claim, const HarnessOptions& options, const platform::CpuInfo& cpu,
    const std::string& host_label, const platform::AffinityState& affinity,
    const std::string& pin_method, const platform::FpState& fp, const platform::TimerReport& timer,
    const results::Provenance& provenance, const std::string& started_at) {
  std::vector<results::HeaderField> header;
  const auto add = [&header](std::string_view key, std::string value) {
    header.push_back({.key = std::string(key), .value = std::move(value)});
  };
  // "none" unless the benchmark says otherwise (RunExtras). Only a benchmark of the project's
  // own code may claim "result"; the self-test claims nothing.
  add("claim", claim);
  add("host_label", host_label);
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
  add("pinning_state", std::string(platform::pinning_state_text(affinity)));
  add("pinned_cpus", affinity.pinned ? platform::format_cpu_list(affinity.cpus) : "none");
  add(results::kMeantIdleCpusKey, options.meant_idle_cpus.empty()
                                      ? "none"
                                      : platform::format_cpu_list(options.meant_idle_cpus) +
                                            std::string(results::kNotEnforced));
  add("pin_method", pin_method);
  add("observed_cpu", affinity.current ? std::to_string(*affinity.current) : "unavailable");
  add("qos_class", affinity.qos_class);
  add("process_nice", std::to_string(platform::process_nice()));
  add("fp_register", std::string(fp.register_name));
  add("fp_state", hex(fp.raw));
  add("fp_ftz", fp.ftz ? "set" : "clear");
  add("fp_daz", fp.daz ? "set" : "clear");
  add("timer_source", timer.tick_source);
  add("timer_calibrated_hz", number(timer.ticks_per_second, "%.1f"));
  add("timer_calibration_spread", number(timer.calibration_spread, "%.6f"));
  add("timer_granularity_ticks", std::to_string(timer.tick_reads.granularity));
  add("timer_min_delta_ticks", std::to_string(timer.tick_reads.min_nonzero_delta));
  add("timer_resolution_ns", number(timer.tick_resolution_ns));
  add("timer_read_cost_ns", number(timer.tick_read_cost_ns));
  add("timer_note", timer.tick_note);
  add("clock_source", timer.clock_source);
  add("clock_getres_ns", number(timer.clock_getres_ns, "%.1f"));
  add("clock_resolution_ns", number(timer.clock_resolution_ns, "%.1f"));
  add("repetitions", std::to_string(options.repetitions));
  add("batches_per_repetition", std::to_string(options.batches));
  add("warmup", "harness " + std::to_string(options.warmup_ms) +
                    " ms, then Google Benchmark min_warmup_time " +
                    number(options.gb_warmup_s, "%g") + " s; samples from both discarded");
  add("commit", provenance.commit);
  add("commit_source", provenance.source);
  add("tree_state", provenance.tree_state);
  add("run_started_at", started_at);
  return header;
}

void add_late_fields(std::vector<results::HeaderField>& header, const platform::FrequencyInfo& freq,
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

}  // namespace

namespace {

bool apply_pin_cpu(std::string_view text, HarnessOptions& options) {
  int cpu = 0;
  if (!parse_integer(text, cpu) || cpu < 0) {
    return false;
  }
  options.pin_cpu = cpu;
  return true;
}

// Recorded, not enforced: nothing stops another process from running there.
bool apply_meant_idle_cpus(std::string_view text, HarnessOptions& options) {
  auto cpus = results::expand_cpu_list(text);
  if (!cpus) {
    return false;
  }
  options.meant_idle_cpus = std::move(*cpus);
  return true;
}

bool apply_source_root(std::string_view text, HarnessOptions& options) {
  options.source_root = std::string(text);
  return true;
}

bool apply_batches(std::string_view text, HarnessOptions& options) {
  return parse_integer(text, options.batches) && options.batches >= 1;
}

bool apply_repetitions(std::string_view text, HarnessOptions& options) {
  return parse_integer(text, options.repetitions) && options.repetitions >= 1;
}

bool apply_warmup_ms(std::string_view text, HarnessOptions& options) {
  return parse_integer(text, options.warmup_ms) && options.warmup_ms >= 0;
}

bool apply_gb_warmup_s(std::string_view text, HarnessOptions& options) {
  const std::string copy(text);
  char* end = nullptr;
  options.gb_warmup_s = std::strtod(copy.c_str(), &end);
  return !copy.empty() && *end == '\0' && options.gb_warmup_s > 0;
}

bool apply_batch_units(std::string_view text, HarnessOptions& options) {
  std::uint64_t value = 0;
  if (!parse_integer(text, value) || value < 1) {
    return false;
  }
  options.batch_units = value;
  return true;
}

struct ValueOption {
  std::string_view flag;
  bool (*apply)(std::string_view, HarnessOptions&);
};

constexpr auto kValueOptions = std::to_array<ValueOption>({
    {.flag = "--pin-cpu=", .apply = apply_pin_cpu},
    {.flag = "--meant-idle-cpus=", .apply = apply_meant_idle_cpus},
    {.flag = "--source-root=", .apply = apply_source_root},
    {.flag = "--batches=", .apply = apply_batches},
    {.flag = "--repetitions=", .apply = apply_repetitions},
    {.flag = "--warmup-ms=", .apply = apply_warmup_ms},
    {.flag = "--gb-warmup-s=", .apply = apply_gb_warmup_s},
    {.flag = "--batch-units=", .apply = apply_batch_units},
});

}  // namespace

std::expected<HarnessOptions, std::string> parse_harness_options(int argc, char** argv) {
  HarnessOptions options;
  if (argc > 0) {
    options.passthrough.emplace_back(argv[0]);
  }
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--reportable") {
      options.reportable = true;
      continue;
    }
    const auto* option = std::ranges::find_if(kValueOptions, [arg](const ValueOption& candidate) {
      return arg.starts_with(candidate.flag);
    });
    if (option == kValueOptions.end()) {
      options.passthrough.emplace_back(arg);
    } else if (!option->apply(arg.substr(option->flag.size()), options)) {
      return std::unexpected("bad option " + std::string(arg));
    }
  }
  return options;
}

std::string render_histogram(std::uint64_t batch_units, const LatencyHistogram& histogram,
                             double timer_resolution_ns) {
  std::string out = "unit ns\n";
  out += "batch_units " + std::to_string(batch_units) + "\n";
  out += "batches " + std::to_string(histogram.count()) + "\n";
  out += "gate_ns " + number(histogram.gate_ns(), "%.1f") + "\n";
  out += "timer_resolution_ns " + number(timer_resolution_ns) + "\n";
  for (const PublishedValue& value : histogram.values()) {
    out += value.label + " " + (value.ns ? std::to_string(*value.ns) : "unresolved") + "\n";
  }
  return out;
}

namespace {

// Warms the workload up, sizes its batch and refuses a batch the timer cannot resolve.
int size_workload(WorkloadRun& run, const HarnessOptions& options, double gate_ns,
                  double target_ns) {
  const double seconds_per_tick = run.seconds_per_tick;
  const auto timed = [&run, seconds_per_tick](std::uint64_t units) {
    const std::uint64_t start = read_ticks();
    run.workload.run(units);
    return static_cast<double>(read_ticks() - start) * seconds_per_tick * 1e9;
  };

  const std::uint64_t warmup_end =
      platform::clock_ns() + static_cast<std::uint64_t>(options.warmup_ms) * 1'000'000U;
  while (platform::clock_ns() < warmup_end) {
    run.workload.run(1);
  }

  if (options.batch_units) {
    run.units = *options.batch_units;
  } else if (run.workload.units != 0) {
    run.units = run.workload.units;
  } else {
    auto sizing = size_batch(timed, target_ns);
    if (!sizing) {
      std::cerr << "harness: " << run.workload.name << ": " << sizing.error() << "\n";
      return kExitHistogramRefused;
    }
    run.units = sizing->units;
  }
  // A fixed-unit workload is timed once: it is a whole pricing, far above the gate.
  const double median = run.workload.units != 0 && !options.batch_units
                            ? timed(run.units)
                            : measure_batch(timed, run.units);
  if (median < gate_ns) {
    std::cerr << "harness: " << run.workload.name << ": refusing to produce a histogram: "
              << "median batch is " << median << " ns, below the " << gate_ns
              << " ns gate; batch more work\n";
    return kExitHistogramRefused;
  }
  return kExitOk;
}

// One histogram section per workload that ran, totalling the counters into perf.
int add_histograms(const std::vector<std::unique_ptr<WorkloadRun>>& runs,
                   const HarnessOptions& options, double gate_ns, double tick_resolution_ns,
                   results::ResultsFile& file, platform::PerfCounterReport& perf) {
  const auto wanted_calls = static_cast<std::size_t>(options.repetitions);
  const auto wanted_batches = static_cast<std::size_t>(options.batches);
  for (const auto& run : runs) {
    if (run->calls.empty()) {
      continue;  // Filtered out by --benchmark_filter
    }
    if (run->calls.size() < wanted_calls) {
      std::cerr << "harness: " << run->workload.name << ": fewer calls than repetitions\n";
      return kExitFailure;
    }
    std::vector<std::int64_t> batch_ns;
    batch_ns.reserve(wanted_calls * wanted_batches);
    for (std::size_t i = run->calls.size() - wanted_calls; i < run->calls.size(); ++i) {
      platform::accumulate(perf, run->call_counters[i]);
      if (run->calls[i].size() != wanted_batches) {
        std::cerr << "harness: " << run->workload.name << ": a repetition is not full length\n";
        return kExitFailure;
      }
      for (const std::uint64_t ticks : run->calls[i]) {
        batch_ns.push_back(std::llround(static_cast<double>(ticks) * run->seconds_per_tick * 1e9));
      }
    }
    auto histogram = LatencyHistogram::from_batches(batch_ns, gate_ns);
    if (!histogram) {
      std::cerr << "harness: " << run->workload.name << ": " << histogram.error() << "\n";
      return kExitHistogramRefused;
    }
    file.sections.push_back({.name = std::string(results::kHistogramPrefix) + run->workload.name,
                             .body = render_histogram(run->units, *histogram, tick_resolution_ns)});
  }
  return kExitOk;
}

}  // namespace

int harness_main(int argc, char** argv, std::vector<Workload> workloads, const RunExtras& extras) {
  auto parsed = parse_harness_options(argc, argv);
  if (!parsed) {
    std::cerr << "harness: " << parsed.error() << "\n";
    return kExitUsage;
  }
  HarnessOptions options = std::move(*parsed);

  const auto fp_before = platform::check_fp_state();
  if (!fp_before) {
    std::cerr << "harness: refusing to run: " << fp_before.error() << "\n";
    return kExitFpState;
  }

  std::string pin_method = "none";
  if (options.pin_cpu) {
    auto pinned = platform::pin_to_cpu(*options.pin_cpu);
    if (!pinned) {
      std::cerr << "harness: " << pinned.error() << "\n";
      return kExitPinFailed;
    }
    pin_method = "--pin-cpu, sched_setaffinity, read back";
  }

  const results::Provenance provenance = results::read_provenance(options.source_root);
  if (options.reportable && provenance.tree_state != "clean" &&
      provenance.tree_state != "exported") {
    std::cerr << "harness: reportable run refused: tree is " << provenance.tree_state << "\n";
    return kExitDirtyTree;
  }

  platform::AffinityState affinity = platform::read_affinity();
  if (affinity.pinned && !options.pin_cpu) {
    pin_method = "inherited mask, read back";
  }

  const std::string started_at = utc_now();
  const platform::CpuInfo cpu = platform::read_cpu_info();
  const std::string host_label(platform::classify_host(cpu.os_name, cpu.cpu_model));
  const platform::TimerReport timer = platform::calibrate_timer();
  if (const std::string problem = platform::timer_problem(timer); !problem.empty()) {
    std::cerr << "harness: timer unusable: " << problem << "\n";
    return kExitFailure;
  }
  const double seconds_per_tick = 1.0 / timer.ticks_per_second;
  const double gate_ns = min_batch_span_ns(timer.tick_resolution_ns);
  const double target_ns = target_batch_span_ns(timer.tick_resolution_ns);

  std::vector<std::unique_ptr<WorkloadRun>> runs;
  for (Workload& workload : workloads) {
    auto run = std::make_unique<WorkloadRun>();
    run->workload = std::move(workload);
    run->seconds_per_tick = seconds_per_tick;
    if (const int refused = size_workload(*run, options, gate_ns, target_ns); refused != kExitOk) {
      return refused;
    }

    WorkloadRun* pointer = run.get();
    benchmark::RegisterBenchmark(
        run->workload.name, [pointer](benchmark::State& state) { run_batches(state, *pointer); })
        ->Iterations(options.batches)
        ->Repetitions(static_cast<int>(options.repetitions))
        ->UseManualTime()
        ->Unit(benchmark::kNanosecond);
    runs.push_back(std::move(run));
  }

  // Goes through argv because of the fixed iteration count; see the header comment.
  options.passthrough.push_back("--benchmark_min_warmup_time=" + number(options.gb_warmup_s, "%g"));
  std::vector<char*> gb_argv;
  gb_argv.reserve(options.passthrough.size() + 1);
  for (std::string& arg : options.passthrough) {
    gb_argv.push_back(arg.data());
  }
  gb_argv.push_back(nullptr);
  int gb_argc = static_cast<int>(gb_argv.size()) - 1;
  benchmark::Initialize(&gb_argc, gb_argv.data());
  if (benchmark::ReportUnrecognizedArguments(gb_argc, gb_argv.data())) {
    return kExitUsage;
  }

  std::ostringstream json_text;
  std::ostringstream json_errors;
  benchmark::JSONReporter reporter;
  reporter.SetOutputStream(&json_text);
  reporter.SetErrorStream(&json_errors);

  const std::size_t ran = benchmark::RunSpecifiedBenchmarks(&reporter);
  if (ran == 0) {
    std::cerr << "harness: no benchmark ran\n" << json_errors.str();
    return kExitFailure;
  }

  const auto fp_after = platform::check_fp_state();
  if (!fp_after) {
    std::cerr << "harness: FP state changed during the run: " << fp_after.error() << "\n";
    return kExitFpState;
  }

  results::ResultsFile file;
  file.sections.push_back(
      {.name = std::string(results::kBenchmarkSection), .body = json_text.str()});
  if (!file.sections.back().body.ends_with('\n')) {
    file.sections.back().body += '\n';
  }

  platform::PerfCounterReport perf;
  if (const int refused =
          add_histograms(runs, options, gate_ns, timer.tick_resolution_ns, file, perf);
      refused != kExitOk) {
    return refused;
  }

  // Read last: the governor, frequency and CPU are the ones the run actually finished under.
  affinity = platform::read_affinity();
  const int report_cpu = affinity.current.value_or(0);
  file.header = build_header(extras.claim, options, cpu, host_label, affinity, pin_method,
                             *fp_after, timer, provenance, started_at);
  file.header.insert(file.header.end(), extras.header.begin(), extras.header.end());
  add_late_fields(file.header, platform::read_frequency_info(report_cpu), perf,
                  platform::peak_rss_bytes());
  file.sections.insert(file.sections.end(), extras.sections.begin(), extras.sections.end());

  auto text = results::write_results_file(file);
  if (!text) {
    std::cerr << "harness: " << text.error() << "\n";
    return kExitFailure;
  }
  const auto missing = results::missing_keys(file, results::kRunHeaderKeys);
  if (!missing.empty()) {
    std::cerr << "harness: header is missing " << missing.front() << "\n";
    return kExitFailure;
  }
  std::cout << *text;
  std::cout.flush();
  return std::cout.good() ? kExitOk : kExitFailure;
}

}  // namespace aadp::bench
