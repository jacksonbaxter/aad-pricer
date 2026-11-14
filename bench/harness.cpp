#include "harness.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <charconv>
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

#include "latency.hpp"
#include "platform/affinity.hpp"
#include "platform/cpu_info.hpp"
#include "platform/fp_state.hpp"
#include "platform/memory_info.hpp"
#include "platform/perf_counters.hpp"
#include "platform/timer.hpp"
#include "results/results_file.hpp"
#include "results/run_header.hpp"

namespace aadp::bench {

static_assert(platform::kMaxPinnedCpus == results::kMaxPinnedCpus,
              "the harness must not call pinned what the stamp refuses");

namespace {

using platform::read_ticks;

std::string number(double value, const char* format = "%.3f") {
  return results::header_number(value, format);
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
    {.flag = "--batches=", .apply = apply_batches},
    {.flag = "--repetitions=", .apply = apply_repetitions},
    {.flag = "--warmup-ms=", .apply = apply_warmup_ms},
    {.flag = "--gb-warmup-s=", .apply = apply_gb_warmup_s},
    {.flag = "--batch-units=", .apply = apply_batch_units},
});

}  // namespace

std::expected<HarnessOptions, std::string> parse_harness_options(int argc, char* const* argv) {
  HarnessOptions options;
  if (argc > 0) {
    options.passthrough.emplace_back(argv[0]);
  }
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto run_flag = results::apply_run_flag(arg, options);
    if (!run_flag) {
      return std::unexpected(run_flag.error());
    }
    if (*run_flag) {
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

int harness_main(int argc, char* const* argv, std::vector<Workload> workloads,
                 const RunExtras& extras) {
  auto parsed = parse_harness_options(argc, argv);
  if (!parsed) {
    std::cerr << "harness: " << parsed.error() << "\n";
    return kExitUsage;
  }
  HarnessOptions options = std::move(*parsed);

  auto started = results::start_run(options);
  if (!started) {
    std::cerr << "harness: " << started.error().message << "\n";
    return started.error().exit_code;
  }
  const results::RunStart& start = *started;
  const platform::TimerReport& timer = start.timer;
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
  const platform::AffinityState affinity = platform::read_affinity();
  const int report_cpu = affinity.current.value_or(0);
  const results::RunShape shape{.claim = extras.claim,
                                .repetitions = options.repetitions,
                                .batches_per_repetition = options.batches,
                                .warmup = "harness " + std::to_string(options.warmup_ms) +
                                          " ms, then Google Benchmark min_warmup_time " +
                                          number(options.gb_warmup_s, "%g") +
                                          " s; samples from both discarded"};
  file.header = results::run_header_fields(start, shape, *fp_after, affinity);
  file.header.insert(file.header.end(), extras.header.begin(), extras.header.end());
  results::add_run_late_fields(file.header, platform::read_frequency_info(report_cpu), perf,
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
