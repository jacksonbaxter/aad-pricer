// MEM-6 for the vector tape, in tests/ad/allocation_test.cpp's binary and with its counting
// allocator: each lane tape is warmed up with one group and one block end, and then counted across
// four blocks of groups and their block-end set-up sweeps, exactly as RunLaneBatches drives them,
// at every lane width this build can run. The expectation, stated before measuring, is zero.
#include <gtest/gtest.h>
#include <hwy/highway.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "ad/allocation_counter.hpp"
#include "ad/cases.hpp"
#include "ad/gradient.hpp"
#include "ad/tape.hpp"
#include "lane_widths.hpp"
#include "pricer/contract.hpp"
#include "pricer/runner.hpp"
#include "simd/lane_kernels.hpp"
#include "simd/lane_path_gradient.hpp"
#include "simd/lane_real.hpp"
#include "simd/lane_runner.hpp"
#include "simd/lane_tape.hpp"
#include "simd/lane_tape_ops.hpp"

namespace aadp::simd {
namespace {

namespace hn = hwy::HWY_NAMESPACE;
using aadp::testing::allocation::allocations;
using aadp::testing::allocation::counting;

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261124;
constexpr std::uint64_t kPaths = 4 * pricer::kBlockPaths;

// The counter can fail: a lane tape that has to grow past its warm-up is seen allocating.
TEST(LaneTapeAllocation, TheCounterSeesALaneTapeGrow) {
  using D = hn::FixedTag<double, 1>;
  LaneTape<D> tape;
  const LaneTapeScope<D> scope(tape);
  const LaneTapeReal<D> x = tape.Register(hn::Set(D(), 1.0));
  const ad::Position mark = tape.GetPosition();
  static_cast<void>(x * 2.0);
  tape.Rewind(mark);
  allocations = 0;
  counting = true;
  LaneTapeReal<D> y = x;
  for (std::size_t i = 0; i < 2 * ad::kChunkEntries; ++i) y = y * 1.0001;
  counting = false;
  tape.Rewind(mark);
  EXPECT_GE(allocations.load(), 1U);
}

template <class D>
std::uint64_t CountAfterWarmUp(const pricer::Contract& c) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  LaneFallbacks fallbacks;
  LaneGradientFn<D> lane_fn(c, ad::testing::kMarket, PricerKernel{}, fallbacks);
  std::vector<LaneReal<D>> normal(c.steps);
  std::vector<LaneReal<D>> crossing(c.steps);
  const LaneUniforms<LaneReal<D>> u{.normal = normal, .crossing = crossing};
  std::array<LaneReal<D>, ad::kGradientStats> stats{};
  std::array<double, ad::kGradientStats> sum{};
  // Warm-up: one group and one block end.
  FillLanes(D(), kSeed, 0, std::span(normal), std::span(crossing));
  lane_fn(u, stats, kLanes);
  lane_fn.EndBlock(sum);

  allocations = 0;
  counting = true;
  for (std::uint64_t group = kLanes; group <= kPaths; group += kLanes) {
    FillLanes(D(), kSeed, group, std::span(normal), std::span(crossing));
    lane_fn(u, stats, kLanes);
    if (group % pricer::kBlockPaths == 0) lane_fn.EndBlock(sum);
    // One allocation already fails the test, so stop at the first.
    if (allocations.load(std::memory_order_relaxed) != 0) break;
  }
  counting = false;
  return allocations.load();
}

TEST(LaneTapeAllocation, NoHeapAllocationAfterWarmUpAtEveryWidth) {
  testing::ForEachWidth([](auto d) {
    using D = decltype(d);
    std::uint64_t total = 0;
    for (const std::uint32_t steps : {50U, 252U}) {
      for (const ad::testing::Case& c : ad::testing::AllCases(steps)) {
        const std::uint64_t counted = CountAfterWarmUp<D>(c.contract);
        total += counted;
        ASSERT_EQ(counted, 0U) << c.name << ", " << steps << " steps, " << hn::MaxLanes(D())
                               << " lanes: allocated after warm-up";
      }
    }
    std::printf(
        "measured, %zu lanes: every product and treatment at 50 and 252 steps, %llu paths each "
        "after warm-up: %llu allocations\n",
        hn::MaxLanes(D()), static_cast<unsigned long long>(kPaths),
        static_cast<unsigned long long>(total));
  });
}

}  // namespace
}  // namespace aadp::simd
