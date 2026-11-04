// The SIMD primal benchmark (BENCHMARKS.md SIMD-1, SIMD-2, MC-4, RNG-4): the scalar pricer against
// the same pricing on Highway lanes, at every width the build has, one Google Benchmark per
// workload and variant, through the harness (harness.hpp).
//
//   aadp_bench_simd [--aadp-workload=<id>[,<id>...]] [--aadp-scale=<d>] [--aadp-require-valid]
//                   [harness and --benchmark_ flags]
//
// Workloads. The pricing ones run pricer::Price (variant scalar) and simd::PriceLanes at 1, 2, 4
// and 8 lanes up to the build's widest (variants lanes-1 ... lanes-N), one thread, the same seed
// and paths. The math one runs each lane kernel over an array (src/simd/simd_pricer.hpp) against
// the scalar kernel over the same array.
//
// Checked before timing, and recorded: every SIMD estimate (mean, SE, batch means) is Price()'s,
// bit for bit, and every lane kernel's output array is the scalar's. The instruction set and lane
// count are in the header (bench_instruction_set, simd_lane_ceiling), so every speedup taken from
// the file has its ceiling beside it.
//
// --aadp-scale=<d> runs 1 / d of each workload's size: an iteration run, marked as such, and never
// --reportable. --aadp-require-valid exits 9 after writing the file if a check failed.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "build_info.hpp"
#include "harness.hpp"
#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "pricer/contract.hpp"
#include "pricer/normal_cdf.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "simd/simd_pricer.hpp"

namespace {

using aadp::pricer::Contract;
using aadp::pricer::Market;

constexpr int kExitInvalid = 9;
// A development seed: never 1729 or 271828, which the oracle plan reserves.
constexpr std::uint64_t kSeed = 20261119;
constexpr std::uint32_t kBatches = 50;
constexpr std::size_t kMathValues = 1U << 20;

enum class Kind : std::uint8_t { kPricing, kMath };

struct Workload {
  std::string_view id;
  std::string_view targets;
  std::string_view description;
  Kind kind = Kind::kPricing;
  Contract contract;
  Market<double> market{};
  std::uint64_t paths = 0;
};

Contract European() {
  Contract c;
  c.product = aadp::pricer::Product::kEuropean;
  c.strike = 100.0;
  c.steps = 1;
  return c;
}

Contract Asian252() {
  Contract c;
  c.product = aadp::pricer::Product::kAsianArithmetic;
  c.strike = 100.0;
  c.steps = 252;
  return c;
}

// The AAD-1 contract under one-step survival.
Contract OneStepBarrier() {
  Contract c;
  c.product = aadp::pricer::Product::kBarrier;
  c.strike = 120.0;
  c.steps = 156;
  c.barrier = {.direction = aadp::pricer::Direction::kUp,
               .knock = aadp::pricer::Knock::kOut,
               .monitoring = aadp::pricer::Monitoring::kDiscrete,
               .treatment = aadp::pricer::Treatment::kOneStep,
               .level = 150.0,
               .smoothing_width = 0.0};
  return c;
}

const std::vector<Workload>& Workloads() {
  static const std::vector<Workload> workloads = {
      {.id = "simd-european",
       .targets = "MC-4, SIMD-1, SIMD-2",
       .description = "European call, K = S0 = 100, vol 20%, r 3%, T = 1, one exact GBM step, "
                      "1,000,000 paths; MC-4's step count and transform are not stated",
       .contract = European(),
       .market = {.spot = 100.0, .sigma = 0.2, .r = 0.03, .q = 0.0, .maturity = 1.0},
       .paths = 1'000'000},
      {.id = "simd-asian252",
       .targets = "MC-1, SIMD-1, SIMD-2",
       .description = "MC-1's arithmetic Asian: 252 fixings, K = 100, vol 20%, r 3%, T = 1, "
                      "300,000 paths, draws inline",
       .contract = Asian252(),
       .market = {.spot = 100.0, .sigma = 0.2, .r = 0.03, .q = 0.0, .maturity = 1.0},
       .paths = 300'000},
      {.id = "simd-barrier-onestep",
       .targets = "SIMD-1, SIMD-2",
       .description = "AAD-1's up-and-out call (K 120, H 150, 156 dates, vol 20%, r 3%, T 3) "
                      "under one-step survival, 500,000 paths",
       .contract = OneStepBarrier(),
       .market = {.spot = 100.0, .sigma = 0.2, .r = 0.03, .q = 0.0, .maturity = 3.0},
       .paths = 500'000},
      {.id = "simd-math",
       .targets = "RNG-4, SIMD-1, SIMD-2",
       .description = "each math kernel over 2^20 doubles: exp on [-5, 5], log on (0, 4], "
                      "inverse normal on Philox-grid uniforms, normal CDF on [-6, 6]",
       .kind = Kind::kMath,
       .contract = {},
       .market = {},
       .paths = 0},
  };
  return workloads;
}

struct Options {
  std::vector<std::string> workloads;
  std::uint64_t scale = 1;
  bool require_valid = false;
  bool reportable = false;
  std::vector<std::string> rest;
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

bool KnownWorkload(std::string_view id) {
  return std::ranges::any_of(Workloads(), [id](const Workload& w) { return w.id == id; });
}

std::optional<Options> Parse(int argc, const char* const* argv) {
  Options options;
  options.rest.emplace_back(argc > 0 ? argv[0] : "aadp_bench_simd");
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    bool ok = true;
    if (arg.starts_with("--aadp-workload=")) {
      std::string_view list = arg.substr(16);
      while (!list.empty()) {
        const auto comma = list.find(',');
        options.workloads.emplace_back(list.substr(0, comma));
        ok = ok && KnownWorkload(options.workloads.back());
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
      }
    } else if (arg.starts_with("--aadp-scale=")) {
      ok = ParseUnsigned(arg.substr(13), options.scale) && options.scale >= 1;
    } else if (arg == "--aadp-require-valid") {
      options.require_valid = true;
    } else if (arg.starts_with("--aadp-")) {
      ok = false;
    } else {
      options.reportable = options.reportable || arg == "--reportable";
      options.rest.emplace_back(arg);
    }
    if (!ok) {
      std::cerr << "aadp_bench_simd: bad option " << arg << "\n";
      return std::nullopt;
    }
  }
  return options;
}

bool SameBits(double a, double b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

bool SameEstimate(const aadp::pricer::Estimate& a, const aadp::pricer::Estimate& b) {
  return SameBits(a.mean, b.mean) && SameBits(a.standard_error, b.standard_error) &&
         std::ranges::equal(a.batch_means, b.batch_means, SameBits);
}

struct Prepared {
  aadp::bench::RunExtras extras;
  std::vector<aadp::bench::Workload> timed;
  std::vector<std::string> ids;
  std::size_t failed = 0;
  std::string first_failure;
};

void Record(Prepared& out, std::string& body, std::string_view workload, const std::string& check,
            bool passed, const std::string& detail) {
  std::cerr << "aadp_bench_simd: " << workload << ": check " << check << " "
            << (passed ? "pass" : "FAILED") << (detail.empty() ? "" : ": " + detail) << "\n";
  body += "check " + check + ": " + (passed ? "pass" : "FAILED") +
          (detail.empty() ? "" : ", " + detail) + "\n";
  if (!passed && out.failed++ == 0) out.first_failure = std::string(workload) + " " + check;
}

std::optional<int> PreparePricing(const Options& options, const Workload& w, Prepared& out) {
  const std::uint64_t paths = w.paths / options.scale;
  if (paths == 0 || paths % kBatches != 0) {
    std::cerr << "aadp_bench_simd: " << w.id << ": 1/" << options.scale
              << " of its paths is not a whole number per batch\n";
    return aadp::bench::kExitUsage;
  }
  const aadp::pricer::RunConfig config{
      .seed = kSeed, .paths = paths, .batches = kBatches, .threads = 1};
  std::string body = std::string(w.description) + "\ntargets: " + std::string(w.targets) +
                     "\npaths: " + std::to_string(paths) + " in " + std::to_string(kBatches) +
                     " batches, one thread, seed " + std::to_string(kSeed) + "\n";
  const auto scalar = aadp::pricer::Price(w.contract, w.market, config);
  if (!scalar) {
    std::cerr << "aadp_bench_simd: " << w.id << ": Price refused the workload\n";
    return aadp::bench::kExitFailure;
  }
  std::array<char, 64> value{};
  std::snprintf(value.data(), value.size(), "%.17g (SE %.3g)", scalar->mean,
                scalar->standard_error);
  body += "scalar price: ";
  body += value.data();
  body += '\n';
  const std::string name(w.id);
  out.timed.push_back({.name = name + ".scalar",
                       .run =
                           [&w, config](std::uint64_t units) {
                             for (std::uint64_t u = 0; u < units; ++u) {
                               const auto r = aadp::pricer::Price(w.contract, w.market, config);
                               double mean = r->mean;
                               benchmark::DoNotOptimize(mean);
                             }
                           },
                       .units = 1});
  for (const std::size_t lanes : aadp::simd::LaneWidths()) {
    const auto simd = aadp::simd::PriceLanes(w.contract, w.market, config, lanes);
    const std::string variant = "lanes-" + std::to_string(lanes);
    Record(out, body, w.id, variant + "-bits", simd.has_value() && SameEstimate(*simd, *scalar),
           simd ? "mean, SE and " + std::to_string(simd->batch_means.size()) + " batch means"
                : "PriceLanes refused");
    std::string timed_name = name;
    timed_name += '.';
    timed_name += variant;
    out.timed.push_back({.name = timed_name,
                         .run =
                             [&w, config, lanes](std::uint64_t units) {
                               for (std::uint64_t u = 0; u < units; ++u) {
                                 const auto r =
                                     aadp::simd::PriceLanes(w.contract, w.market, config, lanes);
                                 double mean = r->mean;
                                 benchmark::DoNotOptimize(mean);
                               }
                             },
                         .units = 1});
  }
  out.extras.sections.push_back({.name = "simd_" + name, .body = body});
  return std::nullopt;
}

using ArrayFn = bool (*)(std::span<const double>, std::span<double>, std::size_t);

struct MathKernel {
  const char* name;
  double (*scalar)(double);
  ArrayFn lanes;
  double lo;
  double hi;
};

double ScalarExp(double x) { return aadp::math::exp(x); }
double ScalarLog(double x) { return aadp::math::log(x); }
double ScalarInverseNormal(double p) { return aadp::math::inverse_normal(p); }
double ScalarNormalCdf(double x) { return aadp::pricer::NormalCdf(x); }

// Inputs spread evenly over [lo, hi] in a scrambled order, so neighbouring lanes take different
// regions of a select-form kernel. For the inverse normal, Philox-grid uniforms as the pricer
// draws them.
std::vector<double> MathInputs(const MathKernel& k, std::size_t n) {
  std::vector<double> x(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t j = (i * 2654435761U) % n;
    const double t = (static_cast<double>(j) + 0.5) / static_cast<double>(n);
    x[i] = k.lo + (k.hi - k.lo) * t;
  }
  return x;
}

// 1 and the ceiling, once each.
std::vector<std::size_t> MathWidths() {
  std::vector<std::size_t> widths{1};
  if (aadp::simd::MaxLanes() != 1) widths.push_back(aadp::simd::MaxLanes());
  return widths;
}

std::optional<int> PrepareMath(const Options& options, const Workload& w, Prepared& out) {
  const std::size_t n = kMathValues / options.scale;
  if (n == 0) return aadp::bench::kExitUsage;
  static const std::array<MathKernel, 4> kKernels = {{
      {.name = "exp", .scalar = ScalarExp, .lanes = aadp::simd::ExpArray, .lo = -5.0, .hi = 5.0},
      {.name = "log", .scalar = ScalarLog, .lanes = aadp::simd::LogArray, .lo = 0.0, .hi = 4.0},
      {.name = "inverse-normal",
       .scalar = ScalarInverseNormal,
       .lanes = aadp::simd::InverseNormalArray,
       .lo = 0.0,
       .hi = 1.0},
      {.name = "normal-cdf",
       .scalar = ScalarNormalCdf,
       .lanes = aadp::simd::NormalCdfArray,
       .lo = -6.0,
       .hi = 6.0},
  }};
  std::string body = std::string(w.description) + "\ntargets: " + std::string(w.targets) +
                     "\nvalues per call: " + std::to_string(n) + "; time per call, so ns per " +
                     "value is the time over " + std::to_string(n) + "\n";
  for (const MathKernel& k : kKernels) {
    const auto in = std::make_shared<std::vector<double>>(MathInputs(k, n));
    const auto out_scalar = std::make_shared<std::vector<double>>(n);
    for (std::size_t i = 0; i < n; ++i) (*out_scalar)[i] = k.scalar((*in)[i]);
    const std::string name = std::string(w.id) + "." + k.name;
    out.timed.push_back({.name = name + "-scalar",
                         .run =
                             [in, out_scalar, f = k.scalar](std::uint64_t units) {
                               for (std::uint64_t u = 0; u < units; ++u) {
                                 for (std::size_t i = 0; i < in->size(); ++i) {
                                   (*out_scalar)[i] = f((*in)[i]);
                                 }
                                 benchmark::DoNotOptimize(out_scalar->data());
                                 benchmark::ClobberMemory();
                               }
                             },
                         .units = 1});
    // The same lane code at one lane and at the ceiling, so the ratio of the two is the lanes'
    // own speedup and the ratio to the scalar kernel also carries the code difference.
    for (const std::size_t lanes : MathWidths()) {
      const auto out_lanes = std::make_shared<std::vector<double>>(n);
      const std::string suffix = "-lanes-" + std::to_string(lanes);
      std::string check = std::string(k.name);
      check += suffix;
      check += "-bits";
      std::string detail = std::to_string(n);
      detail += " values, ";
      detail += std::to_string(lanes);
      detail += " lanes against the scalar kernel";
      Record(
          out, body, w.id, check,
          k.lanes(*in, *out_lanes, lanes) && std::ranges::equal(*out_scalar, *out_lanes, SameBits),
          detail);
      out.timed.push_back({.name = name + suffix,
                           .run =
                               [in, out_lanes, f = k.lanes, lanes](std::uint64_t units) {
                                 for (std::uint64_t u = 0; u < units; ++u) {
                                   f(*in, *out_lanes, lanes);
                                   benchmark::DoNotOptimize(out_lanes->data());
                                   benchmark::ClobberMemory();
                                 }
                               },
                           .units = 1});
    }
  }
  out.extras.sections.push_back({.name = "simd_" + std::string(w.id), .body = body});
  return std::nullopt;
}

std::string ScaleText(std::uint64_t scale) {
  if (scale == 1) return "1 (the workloads' full sizes)";
  return "1/" + std::to_string(scale) + " of the workloads' sizes: iteration only, not a result";
}

std::string LaneCeiling() {
  const std::string lanes = std::to_string(aadp::simd::MaxLanes());
  return lanes + " double lanes on " + aadp::simd::TargetName() +
         ": no SIMD speedup over one lane can exceed " + lanes +
         ". NEON is 2 wide, SSE2 2, AVX2 4, AVX-512 8; Zen 1 runs each 256-bit AVX2 operation as "
         "two 128-bit halves, so about 2 lanes' throughput there";
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  const auto parsed = Parse(argc, argv);
  if (!parsed) return aadp::bench::kExitUsage;
  const Options& options = *parsed;
  if (options.reportable && options.scale != 1) {
    std::cerr << "aadp_bench_simd: a reduced size is never reportable\n";
    return aadp::bench::kExitUsage;
  }

  Prepared prepared;
  prepared.extras.claim = "result";
  for (const Workload& w : Workloads()) {
    if (!options.workloads.empty() &&
        std::ranges::find(options.workloads, w.id) == options.workloads.end()) {
      continue;
    }
    const auto status = w.kind == Kind::kMath ? PrepareMath(options, w, prepared)
                                              : PreparePricing(options, w, prepared);
    if (status) return *status;
    prepared.ids.emplace_back(w.id);
  }

  std::string id_list;
  for (const std::string& id : prepared.ids) id_list += (id_list.empty() ? "" : ",") + id;
  auto& extras = prepared.extras;
  const auto add = [&extras](std::string key, std::string value) {
    extras.header.push_back({.key = std::move(key), .value = std::move(value)});
  };
  std::string widths;
  for (const std::size_t lanes : aadp::simd::LaneWidths()) {
    widths += (widths.empty() ? "" : ",") + std::to_string(lanes);
  }
  add("bench_workloads", id_list);
  add("bench_scale", ScaleText(options.scale));
  add("bench_seed", std::to_string(kSeed));
  add("bench_mc_batches", std::to_string(kBatches));
  add("bench_threads", "1 for every variant");
  add("bench_instruction_set",
      aadp::simd::TargetName() +
          " (AADP_SIMD_TARGET=" + std::string(aadp::build_info::simd_target) +
          ", Highway static dispatch); scalar variants are the scalar pricer and kernels of the "
          "same binary");
  add("simd_lane_widths_timed", widths);
  add("simd_lane_ceiling", LaneCeiling());
  add("simd_check", prepared.failed == 0 ? "pass"
                                         : "FAILED: " + std::to_string(prepared.failed) +
                                               " checks, first " + prepared.first_failure);

  std::vector<std::string> storage = options.rest;
  std::vector<char*> rest;
  rest.reserve(storage.size());
  for (std::string& arg : storage) rest.push_back(arg.data());
  const int status = aadp::bench::harness_main(static_cast<int>(rest.size()), rest.data(),
                                               std::move(prepared.timed), extras);
  if (status == aadp::bench::kExitOk && options.require_valid && prepared.failed != 0) {
    std::cerr << "aadp_bench_simd: check failed: " << prepared.first_failure << "\n";
    return kExitInvalid;
  }
  return status;
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_bench_simd: %s\n", error.what());
  return aadp::bench::kExitFailure;
}
