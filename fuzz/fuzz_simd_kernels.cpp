// Check 4 of stage 5a: the SIMD kernels against the scalar ones on arbitrary inputs, at every lane
// width the build can run. Byte 0 picks what is driven by the rest, little-endian:
//
//   0  math     up to 64 f64: every lane kernel (exp, log, sqrt, inverse normal, normal CDF)
//               against its scalar twin, the doubles L at a time at each width L. Any bit
//               pattern, so NaNs of every sign and payload, subnormals and the edges.
//   1  philox   u64 seed, u64 first path, u32 step, then u32 x 4 counter and u32 x 2 key:
//               DrawStepLanes against DrawStep for each lane, and the raw generator on a counter
//               that differs per lane against Philox4x32_10.
//   2  pricer   the 88-byte contract and market of fuzz_pricer.cpp: on a valid contract of at most
//               64 steps, EvaluateLanes per lane against EvaluatePath, and PriceLanes against
//               Price() at 64 paths in 4 batches, on 1 to 3 threads.
//
// Every comparison is of the bits, NaN included. A mismatch aborts and says what it was.
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

#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/normal_cdf.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "rng/philox.hpp"
#include "simd/lane_kernels.hpp"
#include "simd/lane_math.hpp"
#include "simd/lane_real.hpp"
#include "simd/lane_runner.hpp"
#include "simd/philox_lanes.hpp"
#include "simd/simd_pricer.hpp"

namespace {

namespace hn = hwy::HWY_NAMESPACE;
using namespace aadp;

constexpr std::size_t kTargetLanes = hn::MaxLanes(hn::ScalableTag<double>());

// width is the lane count, at the input or path where the bits differ.
[[noreturn]] void Fail(const char* what, std::size_t width, std::uint64_t at, std::uint64_t got,
                       std::uint64_t want) {
  std::fprintf(stderr,
               "simd mismatch: %s at %zu lanes: input 0x%016llx, lane 0x%016llx, scalar "
               "0x%016llx\n",
               what, width, static_cast<unsigned long long>(at),
               static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
  std::abort();
}

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

template <class T>
T Read(std::span<const std::uint8_t> data, std::size_t offset) {
  T value{};
  if (offset + sizeof(T) <= data.size()) std::memcpy(&value, data.data() + offset, sizeof(T));
  return value;
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

template <class D, class LaneFn, class ScalarFn>
void CheckKernel(D d, const char* name, std::span<const double> x, const LaneFn& lane_fn,
                 const ScalarFn& scalar_fn) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  std::array<double, kLanes> in{};
  std::array<double, kLanes> out{};
  for (std::size_t start = 0; start < x.size(); start += kLanes) {
    for (std::size_t lane = 0; lane < kLanes; ++lane) in[lane] = x[(start + lane) % x.size()];
    hn::StoreU(lane_fn(d, hn::LoadU(d, in.data())), d, out.data());
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      const double want = scalar_fn(in[lane]);
      if (Bits(out[lane]) != Bits(want)) {
        Fail(name, kLanes, Bits(in[lane]), Bits(out[lane]), Bits(want));
      }
    }
  }
}

void FuzzMath(std::span<const std::uint8_t> data) {
  std::vector<double> x;
  for (std::size_t offset = 0; offset + 8 <= data.size() && x.size() < 64; offset += 8) {
    x.push_back(Read<double>(data, offset));
  }
  if (x.empty()) return;
  ForEachWidth([&](auto d) {
    CheckKernel(
        d, "exp", x, [](auto t, auto v) { return simd::vmath::Exp(t, v); },
        [](double v) { return math::exp(v); });
    CheckKernel(
        d, "log", x, [](auto t, auto v) { return simd::vmath::Log(t, v); },
        [](double v) { return math::log(v); });
    CheckKernel(
        d, "sqrt", x, [](auto t, auto v) { return simd::vmath::Sqrt(t, v); },
        [](double v) { return math::sqrt(v); });
    CheckKernel(
        d, "inverse normal", x, [](auto t, auto v) { return simd::vmath::InverseNormal(t, v); },
        [](double v) { return math::inverse_normal(v); });
    CheckKernel(
        d, "normal cdf", x, [](auto t, auto v) { return simd::vmath::NormalCdf(t, v); },
        [](double v) { return pricer::NormalCdf(v); });
  });
}

template <class D>
void CheckDraws(D d, std::uint64_t seed, std::uint64_t first, std::uint32_t step) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  const simd::StepUniformLanes<D> u = simd::DrawStepLanes(d, seed, first, step);
  std::array<double, kLanes> normal{};
  std::array<double, kLanes> crossing{};
  hn::StoreU(u.normal, d, normal.data());
  hn::StoreU(u.crossing, d, crossing.data());
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    const pricer::StepUniforms s = pricer::DrawStep(seed, first + lane, step);
    if (Bits(normal[lane]) != Bits(s.normal)) {
      Fail("philox normal uniform", kLanes, first + lane, Bits(normal[lane]), Bits(s.normal));
    }
    if (Bits(crossing[lane]) != Bits(s.crossing)) {
      Fail("philox crossing uniform", kLanes, first + lane, Bits(crossing[lane]), Bits(s.crossing));
    }
  }
}

// The raw generator, lane i on counter word 0 plus i, against Philox4x32_10.
template <class D>
void CheckGenerator(D /*d*/, const Philox4x32Counter& counter, const Philox4x32Key& key) {
  using DU = hn::RebindToUnsigned<D>;
  const DU du;
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  std::array<std::array<std::uint64_t, kLanes>, 4> words{};
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    for (std::size_t w = 0; w < 4; ++w) words[w][lane] = counter[w];
    words[0][lane] = static_cast<std::uint32_t>(counter[0] + lane);
  }
  const simd::PhiloxWords<DU> c = {hn::LoadU(du, words[0].data()), hn::LoadU(du, words[1].data()),
                                   hn::LoadU(du, words[2].data()), hn::LoadU(du, words[3].data())};
  const simd::PhiloxKeyWords<DU> k = {hn::Set(du, std::uint64_t{key[0]}),
                                      hn::Set(du, std::uint64_t{key[1]})};
  const simd::PhiloxWords<DU> out = simd::Philox4x32_10Lanes(du, c, k);
  for (std::size_t w = 0; w < 4; ++w) hn::StoreU(out[w], du, words[w].data());
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    Philox4x32Counter lane_counter = counter;
    lane_counter[0] = static_cast<std::uint32_t>(counter[0] + lane);
    const Philox4x32Output want = Philox4x32_10(lane_counter, key);
    for (std::size_t w = 0; w < 4; ++w) {
      if (words[w][lane] != want[w]) Fail("philox word", kLanes, w, words[w][lane], want[w]);
    }
  }
}

void FuzzPhilox(std::span<const std::uint8_t> data) {
  const auto seed = Read<std::uint64_t>(data, 0);
  const auto first = Read<std::uint64_t>(data, 8);
  const auto step = Read<std::uint32_t>(data, 16);
  Philox4x32Counter counter{};
  Philox4x32Key key{};
  for (std::size_t w = 0; w < 4; ++w) counter[w] = Read<std::uint32_t>(data, 20 + (4 * w));
  for (std::size_t w = 0; w < 2; ++w) key[w] = Read<std::uint32_t>(data, 36 + (4 * w));
  ForEachWidth([&](auto d) {
    CheckDraws(d, seed, first, step);
    CheckGenerator(d, counter, key);
  });
}

// fuzz_pricer.cpp's layout, from data[0].
struct Input {
  pricer::Contract contract;
  pricer::Market<double> market{};
  std::uint64_t seed = 0;
  std::uint32_t threads = 1;
};

Input Decode(std::span<const std::uint8_t> data) {
  using namespace pricer;
  Input in;
  Contract& c = in.contract;
  c.product = static_cast<Product>(Read<std::uint8_t>(data, 0));
  c.type = static_cast<OptionType>(Read<std::uint8_t>(data, 1));
  c.barrier.direction = static_cast<Direction>(Read<std::uint8_t>(data, 2));
  c.barrier.knock = static_cast<Knock>(Read<std::uint8_t>(data, 3));
  c.barrier.monitoring = static_cast<Monitoring>(Read<std::uint8_t>(data, 4));
  c.barrier.treatment = static_cast<Treatment>(Read<std::uint8_t>(data, 5));
  c.include_spot = (Read<std::uint8_t>(data, 6) & 1U) != 0;
  in.threads = 1U + (Read<std::uint8_t>(data, 7) % 3U);
  c.steps = Read<std::uint32_t>(data, 8);
  in.seed = Read<std::uint64_t>(data, 16);
  const auto field = [&](std::size_t i) { return Read<double>(data, 24 + (8 * i)); };
  in.market.spot = field(0);
  c.strike = field(1);
  c.barrier.level = field(2);
  c.barrier.smoothing_width = field(3);
  in.market.sigma = field(4);
  in.market.r = field(5);
  in.market.q = field(6);
  in.market.maturity = field(7);
  return in;
}

template <class D>
void CheckPaths(D d, const Input& in) {
  using Real = simd::LaneReal<D>;
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  const pricer::Contract& c = in.contract;
  const pricer::GbmModel<double> scalar_model(in.market, c.steps);
  const simd::LaneGbm<Real> model(scalar_model);
  std::vector<Real> normal(c.steps);
  std::vector<Real> crossing(pricer::NeedsCrossingUniforms(c) ? c.steps : 0);
  simd::FillLanes(d, in.seed, 0, std::span(normal), std::span(crossing));
  const simd::LaneUniforms<Real> u{.normal = normal, .crossing = crossing};
  const pricer::PathValue<Real> lanes = simd::EvaluateLanes<Real>(c, model, u);
  std::array<double, kLanes> values{};
  std::array<double, kLanes> weights{};
  hn::StoreU(lanes.value.vec(), d, values.data());
  hn::StoreU(lanes.weight.vec(), d, weights.data());
  std::vector<double> scalar_normal(c.steps);
  std::vector<double> scalar_crossing(c.steps);
  const pricer::PathUniforms scalar_u{.normal = scalar_normal, .crossing = scalar_crossing};
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    pricer::FillPath(in.seed, lane, scalar_normal, scalar_crossing);
    const auto want = pricer::EvaluatePath<double>(c, scalar_model, scalar_u);
    if (Bits(values[lane]) != Bits(want.value)) {
      Fail("path value", kLanes, lane, Bits(values[lane]), Bits(want.value));
    }
    if (Bits(weights[lane]) != Bits(want.weight)) {
      Fail("path weight", kLanes, lane, Bits(weights[lane]), Bits(want.weight));
    }
  }
}

void FuzzPricer(std::span<const std::uint8_t> data) {
  constexpr std::size_t kInputBytes = 88;
  constexpr std::uint32_t kMaxSteps = 64;
  if (data.size() < kInputBytes) return;
  const Input in = Decode(data);
  if (pricer::Validate(in.contract, in.market) != pricer::ContractError::kNone) return;
  if (in.contract.steps > kMaxSteps) return;
  ForEachWidth([&](auto d) { CheckPaths(d, in); });
  const pricer::RunConfig config{.seed = in.seed, .paths = 64, .batches = 4, .threads = in.threads};
  const auto want = pricer::Price(in.contract, in.market, config);
  if (!want) Fail("Price refused a valid contract", 1, 0, 0, 0);
  for (const std::size_t lanes : simd::LaneWidths()) {
    const auto got = simd::PriceLanes(in.contract, in.market, config, lanes);
    if (!got) Fail("PriceLanes refused a valid contract", lanes, 0, 0, 0);
    if (Bits(got->mean) != Bits(want->mean)) {
      Fail("price", lanes, in.seed, Bits(got->mean), Bits(want->mean));
    }
    if (Bits(got->standard_error) != Bits(want->standard_error)) {
      Fail("standard error", lanes, in.seed, Bits(got->standard_error), Bits(want->standard_error));
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 1) return 0;
  const std::span<const std::uint8_t> rest(data + 1, size - 1);
  switch (data[0] % 3) {
    case 0:
      FuzzMath(rest);
      break;
    case 1:
      FuzzPhilox(rest);
      break;
    default:
      FuzzPricer(rest);
      break;
  }
  return 0;
}
