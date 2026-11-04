// Check 1 of stage 5a, path by path: lane i of EvaluateLanes on LaneReal gives the value and the
// weight pricer::EvaluatePath<double> gives path first + i, bit for bit, on every product and
// treatment, at every width this build can run, on draws from the lane Philox. And the lane
// kernels with Real = double are the scalar kernels again, bit for bit.
//
// Three markets: a plain one; a wild one (high volatility, long maturity), which reaches both
// tails of AS241 and the far regions of the normal CDF; and a dying one, whose drift carries every
// one-step path through the barrier, so p (1 - u) underflows to zero and the dead-path Selects run.
#include <gtest/gtest.h>
#include <hwy/highway.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "cases.hpp"
#include "lane_widths.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "simd/lane_kernels.hpp"
#include "simd/lane_real.hpp"
#include "simd/lane_runner.hpp"

#ifndef AADP_SMOKE_SIZE
#define AADP_SMOKE_SIZE 0
#endif

namespace aadp::simd {
namespace {

namespace hn = hwy::HWY_NAMESPACE;
using pricer::Contract;
using pricer::Market;
using testing::ForEachWidth;
using testing::SameBits;

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261116;
constexpr std::uint64_t kPaths = AADP_SMOKE_SIZE ? 64 : 2048;
// Starts off a multiple of every width, so lane 0 is not always an even path.
constexpr std::uint64_t kFirstPath = 4093;

struct NamedMarket {
  const char* name;
  Market<double> market;
  std::uint32_t steps;
};

constexpr auto kMarkets = std::to_array<NamedMarket>({
    {.name = "plain",
     .market = {.spot = 100.0, .sigma = 0.3, .r = 0.05, .q = 0.02, .maturity = 1.0},
     .steps = 12},
    {.name = "wild",
     .market = {.spot = 100.0, .sigma = 1.2, .r = 0.1, .q = 0.0, .maturity = 2.0},
     .steps = 7},
    {.name = "dying",
     .market = {.spot = 100.0, .sigma = 1e-4, .r = -1.0, .q = 1.0, .maturity = 1.0},
     .steps = 12},
});

struct Tally {
  std::size_t paths = 0;
  std::size_t mismatches = 0;
};

template <class D>
void CompareCase(D d, const Contract& c, const Market<double>& market, const std::string& name,
                 Tally& tally) {
  using Real = LaneReal<D>;
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  const pricer::GbmModel<double> scalar_model(market, c.steps);
  const LaneGbm<Real> model(scalar_model);
  std::vector<Real> normal(c.steps);
  std::vector<Real> crossing(pricer::NeedsCrossingUniforms(c) ? c.steps : 0);
  const LaneUniforms<Real> lane_u{.normal = normal, .crossing = crossing};
  std::vector<double> scalar_normal(c.steps);
  std::vector<double> scalar_crossing(c.steps);
  const pricer::PathUniforms scalar_u{.normal = scalar_normal, .crossing = scalar_crossing};
  std::array<double, kLanes> values{};
  std::array<double, kLanes> weights{};
  for (std::uint64_t group = kFirstPath; group < kFirstPath + kPaths; group += kLanes) {
    FillLanes(d, kSeed, group, std::span(normal), std::span(crossing));
    const pricer::PathValue<Real> lanes = EvaluateLanes<Real>(c, model, lane_u);
    hn::StoreU(lanes.value.vec(), d, values.data());
    hn::StoreU(lanes.weight.vec(), d, weights.data());
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      pricer::FillPath(kSeed, group + lane, scalar_normal, scalar_crossing);
      const pricer::PathValue<double> expected =
          pricer::EvaluatePath<double>(c, scalar_model, scalar_u);
      ++tally.paths;
      if (!SameBits(values[lane], expected.value) || !SameBits(weights[lane], expected.weight)) {
        if (tally.mismatches++ < 5) {
          ADD_FAILURE() << name << ", " << kLanes << " lanes, path " << group + lane
                        << ": lane value " << values[lane] << " weight " << weights[lane]
                        << "; scalar " << expected.value << " weight " << expected.weight;
        }
      }
    }
  }
}

TEST(SimdPaths, EveryProductIsBitIdenticalOnEveryLane) {
  for (const NamedMarket& m : kMarkets) {
    const auto cases = testing::AllCases(m.steps);
    ForEachWidth([&](auto d) {
      constexpr std::size_t kLanes = hn::MaxLanes(decltype(d)());
      Tally tally;
      for (const testing::Case& c : cases) {
        ASSERT_EQ(pricer::Validate(c.contract, m.market), pricer::ContractError::kNone)
            << c.name << " on " << m.name;
        CompareCase(d, c.contract, m.market, c.name + " on " + m.name, tally);
      }
      std::cout << "simd paths, " << m.name << " market: " << kLanes << " lanes, " << cases.size()
                << " cases, " << tally.paths << " paths, " << tally.mismatches
                << " not bit-identical\n";
      EXPECT_EQ(tally.mismatches, 0U);
    });
  }
}

// The lane kernels are a copy of kernels.hpp with the uniforms' type changed. With Real = double
// they must be kernels.hpp, so the copy cannot drift from it unseen.
TEST(SimdPaths, LaneKernelsOnDoubleAreTheScalarKernels) {
  for (const NamedMarket& m : kMarkets) {
    for (const testing::Case& c : testing::AllCases(m.steps)) {
      const pricer::GbmModel<double> scalar_model(m.market, c.contract.steps);
      const LaneGbm<double> model(scalar_model);
      std::vector<double> normal(c.contract.steps);
      std::vector<double> crossing(c.contract.steps);
      const pricer::PathUniforms scalar_u{.normal = normal, .crossing = crossing};
      const LaneUniforms<double> lane_u{.normal = normal, .crossing = crossing};
      std::size_t mismatches = 0;
      for (std::uint64_t path = 0; path < kPaths; ++path) {
        pricer::FillPath(kSeed, path, normal, crossing);
        const auto expected = pricer::EvaluatePath<double>(c.contract, scalar_model, scalar_u);
        const auto got = EvaluateLanes<double>(c.contract, model, lane_u);
        if (!SameBits(got.value, expected.value) || !SameBits(got.weight, expected.weight)) {
          ++mismatches;
        }
      }
      EXPECT_EQ(mismatches, 0U) << c.name << " on " << m.name;
    }
  }
}

}  // namespace
}  // namespace aadp::simd
