// The vector tape against the scalar tape on arbitrary contracts and markets (EVIDENCE B23, and
// docs/NOTES.md, "Vector tape: design and oracle"), at every lane width the build can run. It
// shares fuzz_pricer.cpp's input layout and seed corpus: 88 bytes decode to a contract, a market, a
// seed and a thread count.
//
// Properties, every one bit for bit:
// - an invalid contract: PriceWithGradientLanes refuses it for the reason PriceWithGradient does;
// - a valid contract of at most kMaxPricedSteps steps: PriceWithGradientLanes gives
//   PriceWithGradient's estimate (value and five Greeks: mean, SE and every batch mean) on 1 to 3
//   threads, with batches that end inside a group at every width above one, and no fallback;
// - per path, on a few groups: each lane's value and gradient are PathGradient::Evaluate's.
// A mismatch aborts and says what it was.
#include <hwy/highway.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "ad/gradient.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "simd/lane_gradient.hpp"
#include "simd/lane_kernels.hpp"
#include "simd/lane_path_gradient.hpp"
#include "simd/lane_real.hpp"
#include "simd/lane_runner.hpp"
#include "simd/simd_pricer.hpp"

namespace {

namespace hn = hwy::HWY_NAMESPACE;
using namespace aadp;
using namespace aadp::pricer;

constexpr std::size_t kInputBytes = 88;
constexpr std::size_t kTargetLanes = hn::MaxLanes(hn::ScalableTag<double>());
// 4 batches of 37 paths: not a multiple of any width above one.
constexpr std::uint32_t kBatches = 4;
constexpr std::uint64_t kPaths = std::uint64_t{kBatches} * 37;
constexpr std::uint64_t kCheckedGroups = 2;
// The tape is checked on shorter paths than fuzz_pricer.cpp prices, to keep each input fast.
constexpr std::uint32_t kMaxPricedSteps = 64;

[[noreturn]] void Fail(const char* what, std::size_t lanes, std::uint64_t at, double got,
                       double want) {
  std::fprintf(stderr, "lane gradient mismatch: %s at %zu lanes, path or batch %llu: %a, %a\n",
               what, lanes, static_cast<unsigned long long>(at), got, want);
  std::abort();
}

bool SameBits(double a, double b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

template <class T>
T Read(const std::uint8_t* data, std::size_t offset) {
  T value;
  std::memcpy(&value, data + offset, sizeof(T));
  return value;
}

struct Input {
  Contract contract;
  Market<double> market{};
  RunConfig run;
};

// fuzz_pricer.cpp's layout.
Input Decode(const std::uint8_t* data) {
  Input in;
  Contract& c = in.contract;
  c.product = static_cast<Product>(data[0]);
  c.type = static_cast<OptionType>(data[1]);
  c.barrier.direction = static_cast<Direction>(data[2]);
  c.barrier.knock = static_cast<Knock>(data[3]);
  c.barrier.monitoring = static_cast<Monitoring>(data[4]);
  c.barrier.treatment = static_cast<Treatment>(data[5]);
  c.include_spot = (data[6] & 1U) != 0;
  c.steps = Read<std::uint32_t>(data, 8);
  const auto field = [&](std::size_t i) { return Read<double>(data, 24 + (8 * i)); };
  in.market.spot = field(0);
  c.strike = field(1);
  c.barrier.level = field(2);
  c.barrier.smoothing_width = field(3);
  in.market.sigma = field(4);
  in.market.r = field(5);
  in.market.q = field(6);
  in.market.maturity = field(7);
  in.run = {.seed = Read<std::uint64_t>(data, 16),
            .paths = kPaths,
            .batches = kBatches,
            .threads = 1U + (data[7] % 3U)};
  return in;
}

template <std::size_t L, class F>
void CallWidth(const F& f) {
  if constexpr (L <= kTargetLanes) f(hn::FixedTag<double, L>());
}

template <class F>
void ForEachWidth(const F& f) {
  CallWidth<1>(f);
  CallWidth<2>(f);
  CallWidth<4>(f);
  CallWidth<8>(f);
}

void CompareEstimate(const Estimate& got, const Estimate& want, std::size_t lanes,
                     const char* what) {
  if (!SameBits(got.mean, want.mean)) Fail(what, lanes, 0, got.mean, want.mean);
  if (!SameBits(got.standard_error, want.standard_error)) {
    Fail(what, lanes, 0, got.standard_error, want.standard_error);
  }
  for (std::size_t b = 0; b < want.batch_means.size(); ++b) {
    if (!SameBits(got.batch_means[b], want.batch_means[b])) {
      Fail(what, lanes, b, got.batch_means[b], want.batch_means[b]);
    }
  }
}

void CheckEstimates(const Input& in) {
  const auto scalar = ad::PriceWithGradient(in.contract, in.market, in.run);
  if (!scalar.has_value()) Fail("a valid contract was refused", 0, 0, 0.0, 0.0);
  for (const std::size_t lanes : simd::LaneWidths()) {
    const auto got = simd::PriceWithGradientLanes(in.contract, in.market, in.run, lanes);
    if (!got.has_value()) Fail("lanes refused a valid contract", lanes, 0, 0.0, 0.0);
    CompareEstimate(got->estimate.value, scalar->value, lanes, "value estimate");
    for (std::size_t k = 0; k < ad::kInputs; ++k) {
      CompareEstimate(got->estimate.greeks[k], scalar->greeks[k], lanes, "greek estimate");
    }
    if (got->fallback_groups != 0 || got->fallback_paths != 0) {
      Fail("a pricer kernel fell back", lanes, got->fallback_groups, 0.0, 0.0);
    }
  }
}

template <class D>
void CheckPaths(D d, const Input& in) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  const Contract& c = in.contract;
  const std::uint64_t paths = kCheckedGroups * kLanes;
  std::vector<ad::PathGradient::PathResult> scalar;
  {
    ad::PathGradient path(c, in.market);
    std::vector<double> normal(c.steps);
    std::vector<double> crossing(c.steps);
    for (std::uint64_t p = 0; p < paths; ++p) {
      FillPath(in.run.seed, p, normal, crossing);
      scalar.push_back(path.Evaluate({.normal = normal, .crossing = crossing}));
    }
  }
  simd::LanePathGradient<D> lanes(c, in.market);
  std::vector<simd::LaneReal<D>> normal(c.steps);
  std::vector<simd::LaneReal<D>> crossing(c.steps);
  const simd::LaneUniforms<simd::LaneReal<D>> u{.normal = normal, .crossing = crossing};
  for (std::uint64_t group = 0; group < paths; group += kLanes) {
    simd::FillLanes(d, in.run.seed, group, std::span(normal), std::span(crossing));
    const auto result = lanes.Evaluate(u);
    if (result.fell_back) Fail("a pricer kernel's group fell back", kLanes, group, 0.0, 0.0);
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      const ad::PathGradient::PathResult& want = scalar[group + lane];
      if (!SameBits(result.value[lane], want.value)) {
        Fail("path value", kLanes, group + lane, result.value[lane], want.value);
      }
      for (std::size_t k = 0; k < ad::kInputs; ++k) {
        if (!SameBits(result.gradient[lane][k], want.gradient[k])) {
          Fail("path gradient", kLanes, group + lane, result.gradient[lane][k], want.gradient[k]);
        }
      }
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < kInputBytes) return 0;
  const Input in = Decode(data);
  const ContractError error = Validate(in.contract, in.market);
  if (error != ContractError::kNone) {
    for (const std::size_t lanes : simd::LaneWidths()) {
      const auto refused = simd::PriceWithGradientLanes(in.contract, in.market, in.run, lanes);
      if (refused.has_value() || refused.error().price.contract != error) {
        Fail("PriceWithGradientLanes and Validate disagree", lanes, 0, 0.0, 0.0);
      }
    }
    return 0;
  }
  if (in.contract.steps > kMaxPricedSteps) return 0;
  CheckEstimates(in);
  ForEachWidth([&](auto d) { CheckPaths(d, in); });
  return 0;
}
