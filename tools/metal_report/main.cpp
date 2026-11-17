// aadp_metal_report: the stage 9 measurements of docs/NOTES.md, "Stage 9". AAD does not run on the
// GPU; every figure here is a price or the time to price, never a Greek.
//
//   aadp_metal_report --mode=properties
//       the property kernels: arithmetic and the constructed ties, contraction, denormals,
//       uint to float, exp and log (precise, fast and the host's float libm) on the grids,
//       compensation
//   aadp_metal_report --mode=batch
//       asian252, paths per command buffer doubling from 2^14, one buffer at a time, until an
//       error or a buffer over 2 s of GPU time; the safe batch by the registered rule
//   aadp_metal_report --mode=table [--paths=<n>] [--threads=<n>] [--report [run flags]]
//       the four columns and their paired differences, GPU-2 and GPU-3, seed 20261202
//   aadp_metal_report --mode=timing --paths-per-buffer=<n> [--paths=<n>] [--reps=<n>]
//                     [--report [run flags]]
//       GPU-1: GPU wall clock against the SIMD CPU (2 lanes) and the CPU float32 line, on 1 and
//       16 threads, seed 20261203. Run it under measure.sh
//
// Without --report a mode prints text and stamps nothing. With it, the table and the timing write
// a report file (results/report.hpp) on stdout, with the run header and the run flags
// (results/run_header.hpp), for scripts/run_bench.sh to run through measure.sh and stamp; the
// text, the same as without it, goes to stderr, and its "  = " lines give every cell at %.17g.
// What is computed and what is timed are the same in both.
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "f32/contracts.hpp"
#include "f32/cpu_pricer.hpp"
#include "f32/float_kernels.hpp"
#include "f32/paired.hpp"
#include "metal/metal_backend.hpp"
#include "metal/probes.hpp"
#include "pricer/runner.hpp"
#include "results/report.hpp"
#include "results/report_run.hpp"
#include "results/results_file.hpp"
#include "results/run_header.hpp"
#include "simd/simd_pricer.hpp"

namespace {

using aadp::metal::Context;
using aadp::metal::ProbeKernel;
namespace f32 = aadp::f32;
namespace probes = aadp::metal::probes;
namespace results = aadp::results;
using Clock = std::chrono::steady_clock;

double Seconds(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double>(to - from).count();
}

struct Options {
  std::string mode;
  std::uint64_t paths = 1ULL << 24U;
  std::uint64_t paths_per_buffer = 0;
  std::uint32_t threads = 16;
  int reps = 5;
  bool report = false;
  bool run_flags = false;
  results::RunOptions run;
};

bool Parse(int argc, char* const* argv, Options& o) {
  auto rest = results::take_run_flags(argc, argv, o.run);
  if (!rest) {
    std::fprintf(stderr, "%s\n", rest.error().c_str());
    return false;
  }
  o.run_flags = static_cast<int>(rest->size()) + 1 != argc;
  for (const std::string& arg : *rest) {
    const std::string_view a = arg;
    if (a == "--report") {
      o.report = true;
      continue;
    }
    const std::size_t equals = a.find('=');
    if (equals == std::string_view::npos) {
      std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
      return false;
    }
    const std::string_view key = a.substr(0, equals + 1);
    const char* value = arg.c_str() + equals + 1;
    if (key == "--mode=") {
      o.mode = value;
    } else if (key == "--paths=") {
      o.paths = std::strtoull(value, nullptr, 10);
    } else if (key == "--paths-per-buffer=") {
      o.paths_per_buffer = std::strtoull(value, nullptr, 10);
    } else if (key == "--threads=") {
      o.threads = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (key == "--reps=") {
      o.reps = static_cast<int>(std::strtol(value, nullptr, 10));
    } else {
      std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
      return false;
    }
  }
  // The run flags belong to a report file's header, which the text has not got. Only the table
  // and the timing write a report.
  if (o.run_flags && !o.report) {
    std::fprintf(stderr, "a run flag needs --report\n");
    return false;
  }
  if (o.report && o.mode != "table" && o.mode != "timing") {
    std::fprintf(stderr, "only --mode=table and --mode=timing write a report\n");
    return false;
  }
  return !o.mode.empty() && o.reps > 0 && o.threads > 0;
}

// One "  = <table> <key> <value>..." line of the text, every value at %.17g, so that a reader can
// check each cell of the report against the text.
void Values(std::string_view table, const std::string& key, std::initializer_list<double> values) {
  std::printf("  = %s %s", std::string(table).c_str(), key.c_str());
  for (const double v : values) std::printf(" %.17g", v);
  std::printf("\n");
}

std::vector<std::uint32_t> Probe(Context& gpu, ProbeKernel kernel,
                                 const std::vector<std::uint32_t>& in, std::size_t in_words,
                                 std::size_t out_words) {
  const auto threads = static_cast<std::uint32_t>(in.size() / in_words);
  auto out = gpu.Probe(kernel, in, threads * out_words, threads);
  if (!out) throw std::runtime_error("probe failed: " + out.error());
  return *out;
}

float AsFloat(std::uint32_t bits) { return std::bit_cast<float>(bits); }

void PrintFunction(const char* label, probes::Grid grid, std::span<const float> x,
                   std::span<const float> got, bool is_exp) {
  const probes::FunctionError e = is_exp ? probes::ExpError(x, got) : probes::LogError(x, got);
  std::printf("  %-8s %-46s points %llu  max %.4f ULP at x = %.9g  mean %.4f ULP  max abs %.3e\n",
              label, probes::GridName(grid), static_cast<unsigned long long>(e.points), e.max_ulp,
              e.max_ulp_at, e.mean_ulp, e.max_abs);
}

constexpr std::uint32_t kRandom = 1U << 20U;

void Arithmetic(Context& gpu) {
  std::printf("== arithmetic: + - * / sqrt(|a|) against the CPU's round to nearest even\n");
  const auto pairs = probes::ArithmeticInputs(f32::kPropertySeed, kRandom);
  const auto expected = probes::ArithmeticReference(pairs);
  const auto got = Probe(gpu, ProbeKernel::kArithmetic, pairs, 2, 5);
  std::array<int, 5> random_mismatch{};
  for (std::size_t i = 0; i < 5 * std::size_t{kRandom}; ++i) {
    if (got[i] != expected[i]) ++random_mismatch[i % 5];
  }
  std::printf(
      "  random pairs %u: mismatches add %d, subtract %d, multiply %d, divide %d, "
      "sqrt %d\n",
      kRandom, random_mismatch[0], random_mismatch[1], random_mismatch[2], random_mismatch[3],
      random_mismatch[4]);
  const auto names = probes::ConstructedNames();
  constexpr std::array<const char*, 5> kOps = {"+", "-", "*", "/", "sqrt"};
  for (std::size_t c = 0; c < names.size(); ++c) {
    const std::size_t base = 5 * (kRandom + c);
    std::printf("  %s\n", names[c].c_str());
    for (std::size_t op = 0; op < 5; ++op) {
      std::printf("    %-4s gpu %08x %.9g  cpu-rne %08x %.9g  %s\n", kOps.at(op), got[base + op],
                  static_cast<double>(AsFloat(got[base + op])), expected[base + op],
                  static_cast<double>(AsFloat(expected[base + op])),
                  got[base + op] == expected[base + op] ? "same" : "DIFFERENT");
    }
  }
}

void Contraction(Context& gpu) {
  std::printf("== contraction: a * b + c against two roundings\n");
  const auto triples = probes::ContractionInputs(f32::kPropertySeed, kRandom);
  const auto unfused = probes::ContractionReference(triples);
  const auto fused_got = Probe(gpu, ProbeKernel::kContraction, triples, 3, 1);
  int differ = 0;
  int fused_like = 0;
  for (std::size_t i = 0; i < unfused.size(); ++i) {
    if (fused_got[i] != unfused[i]) ++differ;
    const float a = AsFloat(triples[3 * i]);
    const float b = AsFloat(triples[3 * i + 1]);
    const float c = AsFloat(triples[3 * i + 2]);
    if (std::bit_cast<std::uint32_t>(std::fma(a, b, c)) != unfused[i]) ++fused_like;
  }
  std::printf(
      "  triples %u: GPU differs from two roundings in %d; a fused multiply-add would "
      "differ in %d\n",
      kRandom, differ, fused_like);
}

void Denormals(Context& gpu) {
  std::printf("== denormals (FTZ shows as a zero where the CPU keeps the denormal)\n");
  const auto den_in = probes::DenormalInputs();
  const auto den_got = Probe(gpu, ProbeKernel::kPairs, den_in, 2, 4);
  const auto den_names = probes::DenormalNames();
  for (std::size_t i = 0; i < den_names.size(); ++i) {
    const float a = AsFloat(den_in[2 * i]);
    const float b = AsFloat(den_in[2 * i + 1]);
    const float cpu_product = a * b;
    const float cpu_sum = a + b;
    std::printf(
        "  %-30s a*b gpu %08x cpu %08x | a+b gpu %08x cpu %08x | a>0 gpu %u cpu %d | "
        "a!=0 gpu %u cpu %d\n",
        den_names[i].c_str(), den_got[4 * i], std::bit_cast<std::uint32_t>(cpu_product),
        den_got[4 * i + 1], std::bit_cast<std::uint32_t>(cpu_sum), den_got[4 * i + 2],
        a > 0.0F ? 1 : 0, den_got[4 * i + 3], a != 0.0F ? 1 : 0);
  }
}

void UintToFloat(Context& gpu) {
  std::printf("== uint to float\n");
  const auto words = probes::UintInputs(f32::kPropertySeed, kRandom);
  const auto conv = Probe(gpu, ProbeKernel::kUintToFloat, words, 1, 2);
  int conv_mismatch = 0;
  for (std::size_t i = 0; i < words.size(); ++i) {
    if (conv[2 * i] != std::bit_cast<std::uint32_t>(static_cast<float>(words[i]))) {
      ++conv_mismatch;
    }
  }
  for (std::size_t i = 0; i < 5; ++i) {
    const auto cpu = static_cast<float>(words[i]);
    std::printf(
        "  k = %10u (%08x): gpu float(k) %.10g, cpu %.10g; gpu float(k) * 2^-32 = %.10g%s\n",
        words[i], words[i], static_cast<double>(AsFloat(conv[2 * i])), static_cast<double>(cpu),
        static_cast<double>(AsFloat(conv[2 * i + 1])),
        AsFloat(conv[2 * i + 1]) == 1.0F ? "  (u = 1.0)" : "");
  }
  std::printf("  all %zu words: %d differ from the CPU's round to nearest even\n", words.size(),
              conv_mismatch);
}

void Functions(Context& gpu) {
  std::printf("== exp and log against the host's double, in float ULPs of the exact value\n");
  for (const probes::Grid grid : {probes::Grid::kExpWide, probes::Grid::kExpPricer,
                                  probes::Grid::kLogWide, probes::Grid::kLogUnit}) {
    const bool is_exp = grid == probes::Grid::kExpWide || grid == probes::Grid::kExpPricer;
    const std::vector<float> x = probes::GridPoints(grid);
    const auto bits = probes::Bits(x);
    const auto precise =
        probes::Floats(Probe(gpu, is_exp ? ProbeKernel::kExp : ProbeKernel::kLog, bits, 1, 1));
    const auto fast = probes::Floats(
        Probe(gpu, is_exp ? ProbeKernel::kExpFast : ProbeKernel::kLogFast, bits, 1, 1));
    std::vector<float> host(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) host[i] = is_exp ? std::exp(x[i]) : std::log(x[i]);
    PrintFunction("precise", grid, x, precise, is_exp);
    PrintFunction("fast", grid, x, fast, is_exp);
    PrintFunction("host", grid, x, host, is_exp);
    int precise_vs_host = 0;
    for (std::size_t i = 0; i < x.size(); ++i) precise_vs_host += precise[i] != host[i] ? 1 : 0;
    std::printf("  precise differs from the host's float result at %d of %zu points\n",
                precise_vs_host, x.size());
  }
}

void Compensation(Context& gpu) {
  std::printf("== compensation: 1, then 2^16 terms of 0.75 2^-24\n");
  const std::vector<std::uint32_t> comp_in = {std::bit_cast<std::uint32_t>(1.0F),
                                              std::bit_cast<std::uint32_t>(0.75F * 0x1p-24F),
                                              1U << 16U};
  const auto comp = Probe(gpu, ProbeKernel::kCompensated, comp_in, 3, 2);
  std::printf("  compensated %.10g (exact %.10g), plain %.10g\n",
              static_cast<double>(AsFloat(comp[0])), 1.0 + 0.75 * 0x1p-8,
              static_cast<double>(AsFloat(comp[1])));
}

int Properties(Context& gpu) {
  Arithmetic(gpu);
  Contraction(gpu);
  Denormals(gpu);
  UintToFloat(gpu);
  Functions(gpu);
  Compensation(gpu);
  return 0;
}

int Batch(Context& gpu) {
  const f32::Workload& w = f32::kWorkloads[1];
  const auto params = f32::MakeParams(w.contract, w.market, f32::kTimingSeed, f32::kSpan);
  if (!params) return 1;
  std::printf("== batch size on %s: one command buffer per run, doubling from 2^14 paths\n",
              std::string(w.name).c_str());
  std::printf("%14s %14s %14s %s\n", "paths", "gpu_s", "wall_s", "status");
  double longest = 0.0;
  std::vector<std::pair<std::uint64_t, double>> done;
  for (std::uint64_t paths = 1ULL << 14U; paths <= (1ULL << 30U); paths *= 2) {
    const auto run = gpu.Price(*params, paths, paths, false);
    if (!run) {
      std::printf("%14llu %14s %14s error: %s\n", static_cast<unsigned long long>(paths), "-", "-",
                  run.error().c_str());
      break;
    }
    const double gpu_s = run->buffers.front().gpu_s;
    std::printf("%14llu %14.6f %14.6f completed\n", static_cast<unsigned long long>(paths), gpu_s,
                run->wall_s);
    std::fflush(stdout);
    done.emplace_back(paths, gpu_s);
    longest = std::max(longest, gpu_s);
    if (gpu_s > 2.0) break;
  }
  std::uint64_t safe = 0;
  for (const auto& [paths, gpu_s] : done) {
    if (gpu_s <= 0.050 && 16.0 * gpu_s <= longest) safe = std::max(safe, paths);
  }
  std::printf(
      "longest completed buffer %.6f s; safe batch (at most 50 ms and 16 times shorter "
      "than that): %llu paths\n",
      longest, static_cast<unsigned long long>(safe));
  return safe == 0 ? 1 : 0;
}
void PrintEstimate(const char* name, const f32::Estimate& e) {
  std::printf("  %-30s %.9f  SE %.4e\n", name, e.mean, e.standard_error);
}

void PrintDifference(const char* name, const f32::Difference& d, double base) {
  std::printf("  %-30s %+.4e  SE %.4e  relative %+.4e  (SE %.4e)\n", name, d.mean, d.standard_error,
              d.mean / base, d.standard_error / std::fabs(base));
}

// The four columns: A and B double on 32- and 23-bit uniforms, C float32 on the CPU, D float32 on
// the GPU. The differences: B - A, C - B, D - B (GPU-2, GPU-3) and D - C, each against the mean
// of the column it subtracts.
constexpr std::array<std::string_view, 4> kColumns = {"A", "B", "C", "D"};
constexpr std::array<std::string_view, 4> kPairs = {"B-A", "C-B", "D-B", "D-C"};

struct TableRecord {
  std::string workload;
  std::array<f32::Estimate, 4> columns{};
  std::array<f32::Difference, 4> differences{};
  std::array<double, 4> bases{};
  double ratio = 0.0;      // |D - B| / SE_B, GPU-2
  bool pass = false;       // ratio <= kGpu2Factor
  double cpu_ratio = 0.0;  // |C - B| / SE_B, not judged
  std::size_t same = 0;    // Spans whose C and D sums are the same bits
  std::size_t spans = 0;
};

int Table(Context& gpu, const Options& o, std::vector<TableRecord>& records) {
  std::printf("== the stage 9 table: N = %llu per contract, seed %llu, span %u, %u CPU threads\n",
              static_cast<unsigned long long>(o.paths),
              static_cast<unsigned long long>(f32::kTableSeed), f32::kSpan, o.threads);
  int failures = 0;
  for (const f32::Workload& w : f32::kWorkloads) {
    const auto params = f32::MakeParams(w.contract, w.market, f32::kTableSeed, f32::kSpan);
    if (!params) return 1;
    const std::uint64_t spans = o.paths / f32::kSpan;
    const double scale = f32::Scale(w.market);
    const auto a = f32::PriceSpansDouble(w.contract, w.market, f32::kTableSeed, spans, f32::kSpan,
                                         f32::UniformBits::k32, o.threads);
    const auto b = f32::PriceSpansDouble(w.contract, w.market, f32::kTableSeed, spans, f32::kSpan,
                                         f32::UniformBits::k23, o.threads);
    const std::vector<f32::SpanSum> c_spans = f32::PriceSpansCpu(*params, 0, spans, o.threads);
    const auto d_run =
        gpu.Price(*params, o.paths, o.paths_per_buffer == 0 ? o.paths : o.paths_per_buffer, true);
    if (!a || !b || !d_run) {
      std::fprintf(stderr, "%s: a column failed\n", std::string(w.name).c_str());
      return 1;
    }
    const f32::Column c = f32::FromFloat(c_spans, f32::kSpan, scale);
    const f32::Column d = f32::FromFloat(d_run->spans, f32::kSpan, scale);
    TableRecord r;
    r.workload = std::string(w.name);
    r.columns = {f32::ColumnEstimate(*a), f32::ColumnEstimate(*b),
                 f32::ToEstimate(f32::Reduce(c_spans, f32::kSpan), scale),
                 f32::ToEstimate(d_run->moments, scale)};
    const f32::Estimate& ea = r.columns[0];
    const f32::Estimate& eb = r.columns[1];
    const f32::Estimate& ec = r.columns[2];
    const f32::Estimate& ed = r.columns[3];
    r.differences = {f32::PairedDifference(*b, *a), f32::PairedDifference(c, *b),
                     f32::PairedDifference(d, *b), f32::PairedDifference(d, c)};
    r.bases = {ea.mean, eb.mean, eb.mean, ec.mean};
    std::printf("-- %s\n", r.workload.c_str());
    PrintEstimate("A double, 32-bit uniforms", ea);
    PrintEstimate("B double, 23-bit uniforms", eb);
    PrintEstimate("C CPU float32", ec);
    PrintEstimate("D GPU float32", ed);
    PrintDifference("B - A", r.differences[0], r.bases[0]);
    PrintDifference("C - B", r.differences[1], r.bases[1]);
    PrintDifference("D - B (GPU-2, GPU-3)", r.differences[2], r.bases[2]);
    PrintDifference("D - C", r.differences[3], r.bases[3]);
    r.ratio = std::fabs(ed.mean - eb.mean) / eb.standard_error;
    r.pass = r.ratio <= f32::kGpu2Factor;
    failures += r.pass ? 0 : 1;
    std::printf("  GPU-2: |D - B| / SE_B = %.5f against %.2f: %s\n", r.ratio, f32::kGpu2Factor,
                r.pass ? "pass" : "FAIL");
    r.cpu_ratio = std::fabs(ec.mean - eb.mean) / eb.standard_error;
    std::printf("  GPU-2 for the CPU line, not judged: |C - B| / SE_B = %.5f\n", r.cpu_ratio);
    for (std::size_t s = 0; s < c_spans.size(); ++s) {
      const f32::SpanSum& cs = c_spans[s];
      const f32::SpanSum& ds = d_run->spans[s];
      const bool bits_equal =
          std::bit_cast<std::uint32_t>(cs.sum) == std::bit_cast<std::uint32_t>(ds.sum) &&
          std::bit_cast<std::uint32_t>(cs.sum_squares) ==
              std::bit_cast<std::uint32_t>(ds.sum_squares);
      r.same += bits_equal ? 1 : 0;
    }
    r.spans = c_spans.size();
    std::printf("  spans whose C and D sums are the same bits: %zu of %zu\n", r.same, r.spans);
    for (std::size_t i = 0; i < kColumns.size(); ++i) {
      Values("price", r.workload + "." + std::string(kColumns[i]),
             {r.columns[i].mean, r.columns[i].standard_error});
    }
    for (std::size_t i = 0; i < kPairs.size(); ++i) {
      const f32::Difference& diff = r.differences[i];
      Values("difference", r.workload + "." + std::string(kPairs[i]),
             {diff.mean, diff.standard_error, r.bases[i]});
    }
    Values("gpu2", r.workload, {r.ratio, r.cpu_ratio});
    records.push_back(std::move(r));
  }
  return failures == 0 ? 0 : 2;
}

double Median(std::vector<double> v) {
  std::ranges::sort(v);
  return v[v.size() / 2];
}

struct Timed {
  double median_s = 0.0;
  double min_s = 0.0;
};

template <class Run>
Timed TimeIt(const char* label, int reps, Run run) {
  run();  // warm-up
  std::vector<double> walls;
  for (int r = 0; r < reps; ++r) {
    const Clock::time_point start = Clock::now();
    run();
    walls.push_back(Seconds(start, Clock::now()));
  }
  const Timed timed{.median_s = Median(walls), .min_s = std::ranges::min(walls)};
  std::printf("  %-44s median %.6f s  min %.6f s  (%d reps after 1 warm-up)\n", label,
              timed.median_s, timed.min_s, reps);
  std::fflush(stdout);
  return timed;
}

// One timed line of GPU-1: the GPU, or a CPU line on a number of threads.
struct TimeRecord {
  std::string workload;
  std::string line;  // gpu, simd or f32
  std::uint32_t threads = 0;
  Timed timed;
  double price = 0.0;
};

struct GpuRecord {
  std::string workload;
  std::size_t buffers = 0;
  double gpu_s = 0.0;  // Median over the timed calls of the summed GPUStartTime to GPUEndTime
  double price = 0.0;
};

struct TimingRecords {
  aadp::metal::SetupTimes setup;
  std::vector<TimeRecord> lines;
  std::vector<GpuRecord> gpu;
};

std::string LineKey(const TimeRecord& r) {
  return r.workload + "." + r.line + (r.line == "gpu" ? "" : "-" + std::to_string(r.threads));
}

int Timing(Context& gpu, const Options& o, TimingRecords& records) {
  if (o.paths_per_buffer == 0) {
    std::fprintf(stderr, "--paths-per-buffer is required: the safe batch from --mode=batch\n");
    return 1;
  }
  const aadp::metal::SetupTimes& setup = gpu.Setup();
  records.setup = setup;
  std::printf("== GPU-1, iteration only: N = %llu, seed %llu, %llu paths per command buffer\n",
              static_cast<unsigned long long>(o.paths),
              static_cast<unsigned long long>(f32::kTimingSeed),
              static_cast<unsigned long long>(o.paths_per_buffer));
  std::printf(
      "  device %s; setup, not in any wall clock below: device %.6f s, compile %.6f s, "
      "pipelines %.6f s\n",
      gpu.DeviceName().c_str(), setup.device_s, setup.compile_s, setup.pipelines_s);
  std::printf("  SIMD CPU: %s, %zu double lanes (the ceiling), double, 52-bit uniforms\n",
              aadp::simd::TargetName().c_str(), aadp::simd::MaxLanes());
  Values("setup", "setup", {setup.device_s, setup.compile_s, setup.pipelines_s});
  for (const f32::Workload& w : f32::kWorkloads) {
    const auto params = f32::MakeParams(w.contract, w.market, f32::kTimingSeed, f32::kSpan);
    if (!params) return 1;
    std::printf("-- %s\n", std::string(w.name).c_str());
    std::vector<double> gpu_sums;
    std::size_t buffers = 0;
    double checksum = 0.0;
    const Timed gpu_timed = TimeIt("GPU float32, allocation to host reduction", o.reps, [&] {
      const auto run = gpu.Price(*params, o.paths, o.paths_per_buffer, false);
      if (!run) throw std::runtime_error("GPU run failed: " + run.error());
      double total = 0.0;
      for (const auto& b : run->buffers) total += b.gpu_s;
      gpu_sums.push_back(total);
      buffers = run->buffers.size();
      checksum = f32::ToEstimate(run->moments, f32::Scale(w.market)).mean;
    });
    std::printf(
        "    breakdown: %zu command buffers, summed GPUStartTime to GPUEndTime median "
        "%.6f s; price %.9f\n",
        buffers, Median(gpu_sums), checksum);
    const std::string name(w.name);
    records.lines.push_back(
        {.workload = name, .line = "gpu", .threads = 0, .timed = gpu_timed, .price = checksum});
    records.gpu.push_back(
        {.workload = name, .buffers = buffers, .gpu_s = Median(gpu_sums), .price = checksum});
    Values("time", LineKey(records.lines.back()), {gpu_timed.median_s, gpu_timed.min_s, checksum});
    Values("breakdown", name, {Median(gpu_sums), checksum});
    for (const std::uint32_t threads : {1U, o.threads}) {
      const aadp::pricer::RunConfig config{
          .seed = f32::kTimingSeed, .paths = o.paths, .batches = 64, .threads = threads};
      const std::string simd_label = "SIMD CPU, 2 lanes, " + std::to_string(threads) + " thr";
      double simd_price = 0.0;
      const Timed simd_timed = TimeIt(simd_label.c_str(), o.reps, [&] {
        const auto e = aadp::simd::PriceLanes(w.contract, w.market, config, 2);
        if (!e) throw std::runtime_error("the SIMD CPU run failed");
        simd_price = e->mean;
      });
      std::printf("    price %.9f\n", simd_price);
      records.lines.push_back({.workload = name,
                               .line = "simd",
                               .threads = threads,
                               .timed = simd_timed,
                               .price = simd_price});
      Values("time", LineKey(records.lines.back()),
             {simd_timed.median_s, simd_timed.min_s, simd_price});
      const std::string f32_label = "CPU float32, " + std::to_string(threads) + " thr";
      double f32_price = 0.0;
      const Timed f32_timed = TimeIt(f32_label.c_str(), o.reps, [&] {
        const auto spans = f32::PriceSpansCpu(*params, 0, o.paths / f32::kSpan, threads);
        f32_price = f32::ToEstimate(f32::Reduce(spans, f32::kSpan), f32::Scale(w.market)).mean;
      });
      std::printf("    price %.9f\n", f32_price);
      records.lines.push_back({.workload = name,
                               .line = "f32",
                               .threads = threads,
                               .timed = f32_timed,
                               .price = f32_price});
      Values("time", LineKey(records.lines.back()),
             {f32_timed.median_s, f32_timed.min_s, f32_price});
    }
  }
  return 0;
}

// --- The report file -----------------------------------------------------------------------

using results::CellKind;
using results::HeaderField;
using results::ReportColumn;
using results::ReportTable;

ReportColumn Real(std::string name, std::string unit = "none") {
  return {.name = std::move(name), .kind = CellKind::kReal, .unit = std::move(unit)};
}
ReportColumn Integer(std::string name) {
  return {.name = std::move(name), .kind = CellKind::kInteger, .unit = "count"};
}
ReportColumn Text(std::string name) {
  return {.name = std::move(name), .kind = CellKind::kText, .unit = "none"};
}
std::string Cell(double x) { return results::real_cell(x); }
std::string Cell(std::uint64_t n) { return results::integer_cell(static_cast<std::int64_t>(n)); }

std::vector<ReportTable> TableTables(const std::vector<TableRecord>& records, std::uint64_t paths) {
  ReportTable prices{
      .name = "metal_prices",
      .title =
          "Stage 9 table, prices only: A and B double, C float32 on the CPU, D float32 on the "
          "GPU, on the same uniforms",
      .targets = {"GPU-2", "GPU-3"},
      .timing = false,
      .uses_chain = false,
      .columns = {Text("workload"), Text("column"), Real("mean"), Real("standard_error"),
                  Integer("paths")},
      .rows = {}};
  ReportTable differences{
      .name = "metal_differences",
      .title =
          "Stage 9 table: paired differences of the prices, each relative to the mean of the "
          "column it subtracts",
      .targets = {"GPU-2", "GPU-3"},
      .timing = false,
      .uses_chain = false,
      .columns = {Text("workload"), Text("pair"), Real("mean"), Real("standard_error"),
                  Real("base"), Real("relative"), Real("relative_se")},
      .rows = {}};
  ReportTable gpu2{
      .name = "metal_gpu2",
      .title =
          "GPU-2: abs(D - B) / SE_B against 0.01, and the CPU float32 line's abs(C - B) / SE_B, "
          "not judged",
      .targets = {"GPU-2"},
      .timing = false,
      .uses_chain = false,
      .columns = {Text("workload"), Real("ratio"), Real("bound"), Text("verdict"),
                  Real("cpu_ratio"), Integer("same_bit_spans"), Integer("spans")},
      .rows = {}};
  for (const TableRecord& r : records) {
    for (std::size_t i = 0; i < kColumns.size(); ++i) {
      const std::string column(kColumns[i]);
      prices.rows.push_back({.key = r.workload + "." + column,
                             .cells = {r.workload, column, Cell(r.columns[i].mean),
                                       Cell(r.columns[i].standard_error), Cell(paths)}});
    }
    for (std::size_t i = 0; i < kPairs.size(); ++i) {
      const std::string pair(kPairs[i]);
      const f32::Difference& d = r.differences[i];
      differences.rows.push_back(
          {.key = r.workload + "." + pair,
           .cells = {r.workload, pair, Cell(d.mean), Cell(d.standard_error), Cell(r.bases[i]),
                     Cell(d.mean / r.bases[i]), Cell(d.standard_error / std::fabs(r.bases[i]))}});
    }
    gpu2.rows.push_back(
        {.key = r.workload,
         .cells = {r.workload, Cell(r.ratio), Cell(f32::kGpu2Factor), r.pass ? "pass" : "FAIL",
                   Cell(r.cpu_ratio), Cell(static_cast<std::uint64_t>(r.same)),
                   Cell(static_cast<std::uint64_t>(r.spans))}});
  }
  return {prices, differences, gpu2};
}

std::vector<ReportTable> TimingTables(const TimingRecords& records, const Options& o) {
  ReportTable time{
      .name = "metal_time",
      .title =
          "GPU-1, prices only: wall clock of one pricing of every path, the GPU in float32 "
          "against the SIMD CPU in double and the CPU float32 line",
      .targets = {"GPU-1"},
      .timing = true,
      .uses_chain = false,
      .columns = {Text("workload"), Text("line"), Integer("threads"), Real("median", "s"),
                  Real("min", "s"), Real("per_gpu"), Real("price"), Integer("paths"),
                  Integer("repetitions")},
      .rows = {}};
  ReportTable breakdown{
      .name = "metal_gpu_breakdown",
      .title = "GPU-1 breakdown: command buffers and their summed GPU time, not in the headline",
      .targets = {"GPU-1"},
      .timing = true,
      .uses_chain = false,
      .columns = {Text("workload"), Integer("buffers"), Integer("paths_per_buffer"),
                  Real("gpu_time", "s"), Real("price")},
      .rows = {}};
  ReportTable setup{
      .name = "metal_setup",
      .title = "GPU-1 setup, in no wall clock above: device and queue, library compile, pipelines",
      .targets = {"GPU-1"},
      .timing = true,
      .uses_chain = false,
      .columns = {Real("device", "s"), Real("compile", "s"), Real("pipelines", "s")},
      .rows = {{.key = "setup",
                .cells = {Cell(records.setup.device_s), Cell(records.setup.compile_s),
                          Cell(records.setup.pipelines_s)}}}};
  for (const TimeRecord& r : records.lines) {
    double gpu_median = 0.0;
    for (const TimeRecord& g : records.lines) {
      if (g.workload == r.workload && g.line == "gpu") gpu_median = g.timed.median_s;
    }
    time.rows.push_back(
        {.key = LineKey(r),
         .cells = {r.workload, r.line, Cell(std::uint64_t{r.threads}), Cell(r.timed.median_s),
                   Cell(r.timed.min_s), Cell(r.timed.median_s / gpu_median), Cell(r.price),
                   Cell(o.paths), Cell(static_cast<std::uint64_t>(o.reps))}});
  }
  for (const GpuRecord& g : records.gpu) {
    breakdown.rows.push_back({.key = g.workload,
                              .cells = {g.workload, Cell(static_cast<std::uint64_t>(g.buffers)),
                                        Cell(o.paths_per_buffer), Cell(g.gpu_s), Cell(g.price)}});
  }
  return {time, breakdown, setup};
}

std::vector<HeaderField> ReportHeader(const Options& o, Context& gpu) {
  const bool table = o.mode == "table";
  std::string tool = "aadp_metal_report --mode=" + o.mode +
                     " --report --paths=" + std::to_string(o.paths) +
                     " --threads=" + std::to_string(o.threads);
  if (o.paths_per_buffer != 0) tool += " --paths-per-buffer=" + std::to_string(o.paths_per_buffer);
  if (!table) tool += " --reps=" + std::to_string(o.reps);
  std::vector<HeaderField> header{
      {.key = "report_tool", .value = tool},
      {.key = "report_inputs",
       .value = "no market data: the stage 9 contracts asian12, asian252 and barrier156 under GBM "
                "(src/f32/contracts.hpp)"},
      {.key = "report_prices_only",
       .value = "every figure is a price, a difference of prices or the time to price; AAD does "
                "not run on the GPU, and no Greek is in this file"},
      {.key = "metal_device", .value = gpu.DeviceName()},
      {.key = "metal_seed",
       .value = std::to_string(table ? f32::kTableSeed : f32::kTimingSeed) + ", span " +
                std::to_string(f32::kSpan)},
      {.key = "metal_paths", .value = std::to_string(o.paths) + " per contract"},
  };
  if (table) {
    header.push_back(
        {.key = "metal_columns",
         .value = "A double on 32-bit uniforms and B double on 23-bit uniforms (the production "
                  "double kernels), C float32 on the CPU, D float32 on the GPU; B, C and D on the "
                  "same uniforms; " +
                  std::to_string(o.threads) + " CPU threads"});
  } else {
    header.push_back(
        {.key = "report_timing",
         .value = "steady_clock; the GPU line from before buffer allocation to after the host "
                  "reduction, across all command buffers; device, compile and pipelines apart and "
                  "in no line; one warm-up call then " +
                  std::to_string(o.reps) + " repetitions, median and min"});
    header.push_back(
        {.key = "metal_lines",
         .value = "gpu: float32 on the GPU, " + std::to_string(o.paths_per_buffer) +
                  " paths per command buffer; simd: simd::PriceLanes in double at 2 lanes on " +
                  aadp::simd::TargetName() + " (ceiling " + std::to_string(aadp::simd::MaxLanes()) +
                  "), 64 batches; f32: the float32 kernel on the CPU; the CPU lines on 1 and " +
                  std::to_string(o.threads) + " threads"});
  }
  return header;
}

// Writes all of `text` to `fd`.
bool WriteAll(int fd, std::string_view text) {
  while (!text.empty()) {
    const ssize_t wrote = ::write(fd, text.data(), text.size());
    if (wrote <= 0) return false;
    text.remove_prefix(static_cast<std::size_t>(wrote));
  }
  return true;
}

int Run(int argc, char* const* argv) {
  Options o;
  if (!Parse(argc, argv, o)) {
    std::fprintf(stderr,
                 "usage: aadp_metal_report --mode=properties|batch|table|timing ...\n"
                 "       aadp_metal_report --mode=table|timing ... --report [run flags]\n");
    return 2;
  }
  if (o.paths == 0 || o.paths % f32::kSpan != 0 || o.paths % 64 != 0) {
    std::fprintf(stderr, "--paths must be a positive multiple of 64\n");
    return 2;
  }
  std::optional<results::ReportRun> session;
  int report_fd = -1;
  if (o.report) {
    // The run's refusals come before any work, as a benchmark binary's do. Then the text goes to
    // stderr and stdout keeps the report alone.
    auto begun = results::ReportRun::begin(o.run);
    if (!begun) {
      std::fprintf(stderr, "aadp_metal_report: %s\n", begun.error().message.c_str());
      return begun.error().exit_code;
    }
    session.emplace(std::move(*begun));
    std::fflush(stdout);
    report_fd = ::dup(STDOUT_FILENO);
    if (report_fd < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
      std::fprintf(stderr, "aadp_metal_report: cannot keep stdout for the report\n");
      return 1;
    }
  }
  auto created = Context::Create();
  if (!created) {
    std::fprintf(stderr, "metal: %s\n", created.error().c_str());
    return 1;
  }
  Context& gpu = **created;
  std::printf("device %s\n", gpu.DeviceName().c_str());
  if (o.mode == "properties") return Properties(gpu);
  if (o.mode == "batch") return Batch(gpu);
  int status = 2;
  std::vector<ReportTable> tables;
  if (o.mode == "table") {
    std::vector<TableRecord> records;
    status = Table(gpu, o, records);
    // A failed GPU-2 is in the file as its verdict; a column that failed leaves no file.
    if (status != 1) tables = TableTables(records, o.paths);
  } else if (o.mode == "timing") {
    TimingRecords records;
    status = Timing(gpu, o, records);
    if (status == 0) tables = TimingTables(records, o);
  } else {
    std::fprintf(stderr, "unknown mode %s\n", o.mode.c_str());
    return 2;
  }
  std::fflush(stdout);
  if (!session || tables.empty()) return status;
  const results::ReportShape shape{
      .run = {.claim = "result",
              .repetitions = o.mode == "table" ? 1 : o.reps,
              .batches_per_repetition = 1,
              .warmup = o.mode == "table" ? "none" : "one call of each line"},
      .header = ReportHeader(o, gpu)};
  auto text = session->finish(shape, tables, std::nullopt);
  if (!text) {
    std::fprintf(stderr, "aadp_metal_report: %s\n", text.error().message.c_str());
    return text.error().exit_code;
  }
  if (!WriteAll(report_fd, *text)) {
    std::fprintf(stderr, "aadp_metal_report: cannot write the report\n");
    return 2;
  }
  return status;
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  return Run(argc, argv);
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_metal_report: %s\n", error.what());
  return 1;
}
