// What a fallback to the scalar tape costs, test-side only: it runs the test-only diverging kernel
// (diverging_kernel.hpp), so it is no product's benchmark and never a result. Through the harness
// (bench/harness.hpp), one thread, at the build's widest lanes, three variants on the same paths:
//   scalar-tape      the kernel on the scalar tape, path by path (ad::GradientPathFn's steps)
//   lanes-diverging  the kernel on the vector tape at a threshold of 0.5: every group whose lanes
//                    fall on both sides falls back, is rewound and runs its paths on the scalar
//                    tape
//   lanes-uniform    the same kernel at a threshold of 2: the same operations, and no lane diverges
// A fallen-back group costs its lane recording, thrown away, and then one scalar path per lane:
// (lanes-diverging - lanes-uniform x (1 - f)) / (f groups), f the share of groups that fell back.
//
//   aadp_simd_fallback_cost [--aadp-scale=<d>] [--aadp-require-valid] [harness flags]
//
// Checked before timing: both lane runs give the scalar tape's estimate bit for bit (value, Greeks,
// SE and batch means), lanes-uniform falls back nowhere, lanes-diverging somewhere above one lane.
#include <benchmark/benchmark.h>
#include <hwy/highway.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ad/cases.hpp"
#include "ad/gradient.hpp"
#include "diverging_kernel.hpp"
#include "harness.hpp"
#include "platform/cpu_info.hpp"
#include "pricer/contract.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "simd/lane_gradient.hpp"
#include "simd/lane_path_gradient.hpp"
#include "simd/simd_pricer.hpp"

HWY_BEFORE_NAMESPACE();

namespace aadp::simd {
namespace {

namespace hn = hwy::HWY_NAMESPACE;
using testing::DivergingKernel;
using testing::ScalarKernelFn;

constexpr int kExitInvalid = 9;
// A development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261127;
constexpr std::uint64_t kPaths = 1U << 17;
constexpr std::uint32_t kBatches = 32;
constexpr std::uint32_t kSteps = 50;
constexpr double kDiverging = 0.5;
constexpr double kUniform = 2.0;

constexpr std::size_t kLanes = hn::MaxLanes(hn::ScalableTag<double>());
using D = hn::FixedTag<double, kLanes>;

struct Options {
  std::uint64_t scale = 1;
  bool require_valid = false;
  std::vector<std::string> rest;
};

bool ParseOptions(int argc, const char* const* argv, Options& out) {
  out.rest.emplace_back(argc > 0 ? argv[0] : "aadp_simd_fallback_cost");
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg.starts_with("--aadp-scale=")) {
      out.scale = 0;
      for (const char c : arg.substr(13)) {
        if (c < '0' || c > '9' || out.scale > 1'000'000) return false;
        out.scale = (out.scale * 10) + static_cast<std::uint64_t>(c - '0');
      }
      if (out.scale == 0) return false;
    } else if (arg == "--aadp-require-valid") {
      out.require_valid = true;
    } else if (arg.starts_with("--aadp-") || arg == "--reportable") {
      return false;  // Never a result.
    } else {
      out.rest.emplace_back(arg);
    }
  }
  return true;
}

ad::GradientEstimate ScalarTape(const pricer::Contract& c, const pricer::RunConfig& config,
                                double threshold) {
  const pricer::PathShape shape{.steps = c.steps, .crossing = false};
  return ad::EstimateFromTable(pricer::RunBatches<ad::kGradientStats>(config, shape, [&] {
    return ScalarKernelFn(c, ad::testing::kMarket, DivergingKernel{.threshold = threshold});
  }));
}

LaneGradientEstimate Lanes(const pricer::Contract& c, const pricer::RunConfig& config,
                           double threshold) {
  return RunLaneGradient(D(), c, ad::testing::kMarket, config,
                         DivergingKernel{.threshold = threshold});
}

bool SameEstimate(const pricer::Estimate& a, const pricer::Estimate& b) {
  const auto same = [](double x, double y) {
    return std::bit_cast<std::uint64_t>(x) == std::bit_cast<std::uint64_t>(y);
  };
  if (!same(a.mean, b.mean) || !same(a.standard_error, b.standard_error)) return false;
  if (a.batch_means.size() != b.batch_means.size()) return false;
  for (std::size_t i = 0; i < a.batch_means.size(); ++i) {
    if (!same(a.batch_means[i], b.batch_means[i])) return false;
  }
  return true;
}

bool SameGradient(const ad::GradientEstimate& a, const ad::GradientEstimate& b) {
  bool ok = SameEstimate(a.value, b.value);
  for (std::size_t i = 0; i < a.greeks.size(); ++i) {
    ok = ok && SameEstimate(a.greeks[i], b.greeks[i]);
  }
  return ok;
}

int Main(int argc, const char* const* argv) {
  Options options;
  if (!ParseOptions(argc, argv, options) || kPaths % options.scale != 0 ||
      (kPaths / options.scale) % kBatches != 0) {
    std::cerr << "aadp_simd_fallback_cost: bad option; --aadp-scale must divide the paths into "
                 "whole batches, and the run is never --reportable\n";
    return bench::kExitUsage;
  }
  const pricer::Contract c =
      ad::testing::Plain(pricer::Product::kEuropean, pricer::OptionType::kCall, kSteps);
  const pricer::RunConfig config{
      .seed = kSeed, .paths = kPaths / options.scale, .batches = kBatches, .threads = 1};

  const ad::GradientEstimate scalar_diverging = ScalarTape(c, config, kDiverging);
  const ad::GradientEstimate scalar_uniform = ScalarTape(c, config, kUniform);
  const LaneGradientEstimate diverging = Lanes(c, config, kDiverging);
  const LaneGradientEstimate uniform = Lanes(c, config, kUniform);
  std::string body;
  std::size_t failed = 0;
  const auto check = [&](const char* name, bool passed) {
    std::cerr << "aadp_simd_fallback_cost: check " << name << (passed ? " pass" : " FAILED")
              << "\n";
    body += std::string("check ") + name + (passed ? " pass\n" : " FAILED\n");
    failed += passed ? 0 : 1;
  };
  check("lanes-diverging-bits", SameGradient(diverging.estimate, scalar_diverging));
  check("lanes-uniform-bits", SameGradient(uniform.estimate, scalar_uniform));
  check("lanes-uniform-no-fallback", uniform.fallback_groups == 0 && uniform.fallback_paths == 0);
  check("lanes-diverging-falls-back", kLanes == 1 || diverging.fallback_groups > 0);
  // RunLaneBatches's groups: each block of each batch in groups of kLanes, the last one short.
  std::uint64_t groups = 0;
  const std::uint64_t per_batch = config.paths / config.batches;
  for (std::uint64_t first = 0; first < per_batch; first += pricer::kBlockPaths) {
    const std::uint64_t size = std::min<std::uint64_t>(pricer::kBlockPaths, per_batch - first);
    groups += config.batches * ((size + kLanes - 1) / kLanes);
  }
  body += "lanes " + std::to_string(kLanes) + "\npaths " + std::to_string(config.paths) +
          "\nsteps " + std::to_string(kSteps) + "\ngroups " + std::to_string(groups) +
          "\nfallback_groups " + std::to_string(diverging.fallback_groups) + "\nfallback_paths " +
          std::to_string(diverging.fallback_paths) + "\n";

  std::vector<bench::Workload> timed;
  const auto add = [&timed](std::string name, auto run) {
    timed.push_back({.name = "lane-fallback." + std::move(name),
                     .run =
                         [run](std::uint64_t units) {
                           for (std::uint64_t u = 0; u < units; ++u) {
                             double mean = run();
                             benchmark::DoNotOptimize(mean);
                           }
                         },
                     .units = 1});
  };
  add("scalar-tape", [c, config] { return ScalarTape(c, config, kDiverging).greeks[0].mean; });
  add("lanes-diverging",
      [c, config] { return Lanes(c, config, kDiverging).estimate.greeks[0].mean; });
  add("lanes-uniform", [c, config] { return Lanes(c, config, kUniform).estimate.greeks[0].mean; });

  bench::RunExtras extras;
  extras.claim = "none";
  const std::string datapath(platform::fp_datapath_note(platform::read_x86_model()));
  extras.header.push_back({.key = "bench_instruction_set",
                           .value = TargetName() + ", " + std::to_string(kLanes) +
                                    " double lanes (lane ceiling " + std::to_string(kLanes) +
                                    (datapath.empty() ? "" : "; " + datapath) +
                                    "); the scalar tape is scalar C++ in the same binary"});
  extras.header.push_back(
      {.key = "bench_scale",
       .value = "test-only diverging kernel, iteration only, never a result; 1/" +
                std::to_string(options.scale) + " of " + std::to_string(kPaths) + " paths"});
  extras.header.push_back(
      {.key = "bench_check", .value = failed == 0 ? "pass" : std::to_string(failed) + " FAILED"});
  extras.sections.push_back({.name = "lane_fallback", .body = body});

  std::vector<char*> rest;
  rest.reserve(options.rest.size());
  for (std::string& arg : options.rest) rest.push_back(arg.data());
  const int status =
      bench::harness_main(static_cast<int>(rest.size()), rest.data(), std::move(timed), extras);
  if (status == bench::kExitOk && options.require_valid && failed != 0) return kExitInvalid;
  return status;
}

}  // namespace
}  // namespace aadp::simd

HWY_AFTER_NAMESPACE();

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  return aadp::simd::Main(argc, argv);
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_simd_fallback_cost: %s\n", error.what());
  return aadp::bench::kExitFailure;
}
