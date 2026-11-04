// The AAD benchmark: bump and revalue, the tape and the hand adjoint against the scalar primal,
// one Google Benchmark per workload and variant, through the harness (harness.hpp). See
// aad_workloads.hpp for the workloads and variants, aad_checks.hpp for what is verified before
// timing, and BENCHMARKS.md, "How Phase 3 numbers are produced", for the commands.
//
//   aadp_bench_aad [--aadp-workload=<id>[,<id>...]] [--aadp-scale=<d>] [--aadp-threads=<n>]
//                  [--aadp-require-valid] [harness and --benchmark_ flags]
//
// --aadp-scale=<d> runs 1 / d of each target's path count: an iteration run, whose file the
// harness marks as such in bench_scale and which may not be --reportable. --aadp-threads sets the
// worker threads of the threaded workloads (default 8). --aadp-require-valid exits 9 after
// writing the file if any check failed, for ctest; without it a failed check is recorded in the
// file and the exit is the harness's own.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aad_checks.hpp"
#include "aad_workloads.hpp"
#include "harness.hpp"
#include "pricer/runner.hpp"
#include "rss_probe.hpp"

namespace {

namespace aad = aadp::bench::aad;

constexpr int kExitInvalid = 9;

struct Options {
  std::vector<std::string> workloads;  // Empty: all
  std::uint64_t scale = 1;
  std::uint32_t threads = 8;
  bool require_valid = false;
  std::optional<std::string> rss_probe;
  bool reportable = false;
  std::vector<std::string> rest;  // argv[0] and everything for the harness
};

bool ParseUnsigned(std::string_view text, std::uint64_t& out) {
  if (text.empty() || text.size() > 12) return false;
  out = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    out = (out * 10) + static_cast<std::uint64_t>(c - '0');
  }
  return true;
}

std::vector<std::string> SplitList(std::string_view text) {
  std::vector<std::string> out;
  while (!text.empty()) {
    const auto comma = text.find(',');
    out.emplace_back(text.substr(0, comma));
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
  }
  return out;
}

std::optional<Options> Parse(int argc, const char* const* argv) {
  Options options;
  options.rest.emplace_back(argc > 0 ? argv[0] : "aadp_bench_aad");
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [arg](std::string_view flag) -> std::optional<std::string_view> {
      return arg.starts_with(flag) ? std::optional(arg.substr(flag.size())) : std::nullopt;
    };
    bool ok = true;
    std::uint64_t number = 0;
    if (const auto v = value("--aadp-workload=")) {
      options.workloads = SplitList(*v);
      for (const std::string& id : options.workloads) ok = ok && aad::FindWorkload(id) != nullptr;
    } else if (const auto scale_text = value("--aadp-scale=")) {
      ok = ParseUnsigned(*scale_text, options.scale) && options.scale >= 1;
    } else if (const auto threads_text = value("--aadp-threads=")) {
      ok = ParseUnsigned(*threads_text, number) && number >= 1 &&
           number <= aadp::pricer::kMaxThreads;
      options.threads = static_cast<std::uint32_t>(number);
    } else if (arg == "--aadp-require-valid") {
      options.require_valid = true;
    } else if (const auto probe_text = value("--aadp-rss-probe=")) {
      options.rss_probe = std::string(*probe_text);
    } else if (arg.starts_with("--aadp-")) {
      ok = false;
    } else {
      options.reportable = options.reportable || arg == "--reportable";
      options.rest.emplace_back(arg);
    }
    if (!ok) {
      std::cerr << "aadp_bench_aad: bad option " << arg << "\n";
      return std::nullopt;
    }
  }
  return options;
}

std::vector<const aad::Workload*> Selected(const Options& options) {
  std::vector<const aad::Workload*> out;
  for (const aad::Workload& w : aad::Workloads()) {
    if (options.workloads.empty() ||
        std::ranges::find(options.workloads, w.id) != options.workloads.end()) {
      out.push_back(&w);
    }
  }
  return out;
}

// The child of an RSS probe: `idle`, or `<workload>.<variant>` run once.
int RunProbeVariant(const Options& options) {
  if (!options.rss_probe) return 2;
  const std::string& what = *options.rss_probe;
  if (what == "idle") return 0;
  const auto dot = what.find('.');
  const aad::Workload* w = aad::FindWorkload(what.substr(0, dot));
  const auto variant =
      dot == std::string::npos ? std::nullopt : aad::ParseVariant(what.substr(dot + 1));
  if (w == nullptr || !variant || !aad::HasVariant(*w, *variant)) return 2;
  const auto config = aad::ConfigFor(*w, options.scale, options.threads);
  if (!config) return 2;
  aad::VariantResult result = aad::MakeRunner(*w, *variant, *config)();
  benchmark::DoNotOptimize(result.value.mean);
  return 0;
}

// Then it reports its own peak, which the parent reads (rss_probe.hpp says why not wait4's).
int RunProbe(const Options& options) {
  const int status = RunProbeVariant(options);
  if (status != 0) return status;
  return aadp::bench::ReportOwnPeakRss() ? 0 : 1;
}

std::string ScaleText(std::uint64_t scale) {
  if (scale == 1) return "1 (the targets' path counts)";
  return "1/" + std::to_string(scale) +
         " of the targets' path counts: iteration only, not a result";
}

// What the run hands the harness, built workload by workload.
struct Prepared {
  aadp::bench::RunExtras extras;
  std::vector<aadp::bench::Workload> timed;
  std::vector<std::string> ids;
  std::size_t failed = 0;
  std::string first_failure;
};

// Peak RSS of a fresh process idle and running each variant once, into the report.
bool ProbeRss(const Options& options, const aad::Workload& w, aad::WorkloadReport& report) {
  const std::string scale = "--aadp-scale=" + std::to_string(options.scale);
  const std::string threads = "--aadp-threads=" + std::to_string(options.threads);
  const auto idle = aadp::bench::ChildPeakRss(options.rest.front(), {"--aadp-rss-probe=idle"});
  if (!idle) {
    std::cerr << "aadp_bench_aad: RSS probe: " << idle.error() << "\n";
    return false;
  }
  report.peak_rss.emplace_back("idle", *idle);
  for (const aad::Variant variant : aad::kVariants) {
    if (!aad::HasVariant(w, variant)) continue;
    const std::string probe =
        "--aadp-rss-probe=" + std::string(w.id) + "." + std::string(aad::VariantName(variant));
    const auto rss = aadp::bench::ChildPeakRss(options.rest.front(), {probe, scale, threads});
    if (!rss) {
      std::cerr << "aadp_bench_aad: RSS probe " << probe << ": " << rss.error() << "\n";
      return false;
    }
    report.peak_rss.emplace_back(std::string(aad::VariantName(variant)), *rss);
  }
  return true;
}

// Verifies one workload before anything is timed, probes its peak RSS, and registers its timed
// variants. On failure, the exit status.
std::optional<int> Prepare(const Options& options, const aad::Workload& w, Prepared& out) {
  const auto config = aad::ConfigFor(w, options.scale, options.threads);
  if (!config) {
    std::cerr << "aadp_bench_aad: " << w.id << ": 1/" << options.scale
              << " of its paths is not a whole number per batch\n";
    return aadp::bench::kExitUsage;
  }
  std::cerr << "aadp_bench_aad: " << w.id << ": verifying " << config->paths << " paths on "
            << config->threads << " threads\n";
  aad::WorkloadReport report = aad::Verify(w, *config);
  for (const aad::CheckResult& c : report.checks) {
    std::cerr << "aadp_bench_aad: " << w.id << ": check " << c.name << " "
              << (c.passed ? "pass" : "FAILED") << ", worst " << c.worst
              << (c.detail.empty() ? "" : ": " + c.detail) << "\n";
    if (!c.passed && out.failed++ == 0) out.first_failure = std::string(w.id) + " " + c.name;
  }
  if (!ProbeRss(options, w, report)) return aadp::bench::kExitFailure;
  for (const aad::Variant variant : aad::kVariants) {
    if (!aad::HasVariant(w, variant)) continue;
    const auto runner = std::make_shared<aad::Runner>(aad::MakeRunner(w, variant, *config));
    out.timed.push_back({.name = aad::BenchmarkName(w, variant),
                         .run =
                             [runner](std::uint64_t units) {
                               for (std::uint64_t u = 0; u < units; ++u) {
                                 aad::VariantResult result = (*runner)();
                                 benchmark::DoNotOptimize(result.value.mean);
                               }
                             },
                         .units = 1});
  }
  out.extras.sections.push_back(
      {.name = "aad_" + std::string(w.id), .body = aad::RenderSection(report)});
  out.ids.emplace_back(w.id);
  return std::nullopt;
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  const auto parsed = Parse(argc, argv);
  if (!parsed) return aadp::bench::kExitUsage;
  const Options& options = *parsed;
  if (options.rss_probe) return RunProbe(options);
  if (options.reportable && options.scale != 1) {
    std::cerr << "aadp_bench_aad: a reduced size is never reportable\n";
    return aadp::bench::kExitUsage;
  }

  Prepared prepared;
  prepared.extras.claim = "result";
  for (const aad::Workload* w : Selected(options)) {
    if (const auto status = Prepare(options, *w, prepared)) return *status;
  }
  aadp::bench::RunExtras& extras = prepared.extras;
  const std::size_t failed = prepared.failed;
  const std::string& first_failure = prepared.first_failure;

  std::string id_list;
  for (const std::string& id : prepared.ids) id_list += (id_list.empty() ? "" : ",") + id;
  const auto add = [&extras](std::string key, std::string value) {
    extras.header.push_back({.key = std::move(key), .value = std::move(value)});
  };
  add("bench_workloads", id_list);
  add("bench_scale", ScaleText(options.scale));
  add("bench_seed", std::to_string(aad::kSeed));
  add("bench_mc_batches", std::to_string(aad::kMcBatches));
  add("bench_threads", std::to_string(options.threads) +
                           " worker threads for aad3-barrier-mt and mc3-asian252-mt, 1 for the "
                           "others; hardware_concurrency " +
                           std::to_string(std::thread::hardware_concurrency()));
  add("bench_thread_setup",
      "pricer::RunBatches: the calling thread and threads - 1 std::jthread workers take blocks "
      "of 1024 paths in turn, one tape per worker, block sums merged in block order, so every "
      "estimate is the same bits for any thread count; no pinning of its own, see pinning_state");
  add("bench_instruction_set",
      "scalar: the pricer, the tape and the hand adjoint are scalar C++, no SIMD kernel exists "
      "until stage 5; simd_target is the build's Highway target only");
  add("bench_fastest_primal",
      "pending stage 5 (SIMD kernels); multiples here are against the "
      "scalar primal");
  add("tape_config",
      "tape v1: one statement per operation, Real 16 B, statement 1 B, operand 12 B (struct of "
      "arrays), chunks of 8192 entries, thread-local active tape, one tape per worker, mark and "
      "rewind per path, set-up swept once per block of 1024 paths; the zero-adjoint skip only in "
      "tape-skip");
  add("bench_rss_probe",
      "peak RSS of a fresh process of this binary (posix_spawn) running one variant once at this "
      "size, as the process reports it (VmHWM on Linux, getrusage on macOS); idle runs nothing");
  add("gradient_check",
      failed == 0 ? "pass"
                  : "FAILED: " + std::to_string(failed) + " checks, first " + first_failure);

  std::vector<char*> rest;
  std::vector<std::string> storage = options.rest;
  rest.reserve(storage.size());
  for (std::string& arg : storage) rest.push_back(arg.data());
  const int status = aadp::bench::harness_main(static_cast<int>(rest.size()), rest.data(),
                                               std::move(prepared.timed), extras);
  if (status == aadp::bench::kExitOk && options.require_valid && failed != 0) {
    std::cerr << "aadp_bench_aad: gradient check failed: " << first_failure << "\n";
    return kExitInvalid;
  }
  return status;
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_bench_aad: %s\n", error.what());
  return aadp::bench::kExitFailure;
}
