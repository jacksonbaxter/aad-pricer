// MEM-6 (EVIDENCE B5): no heap allocation after warm-up. This binary replaces the global
// allocation functions with ones that count while counting is on, warms each worker's tape up
// with one path, and then counts across many paths and block-end set-up sweeps, exactly as
// RunBatches drives them. The expectation, stated before measuring, is zero.
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "ad/gradient.hpp"
#include "ad/real.hpp"
#include "ad/tape.hpp"
#include "cases.hpp"
#include "pricer/draws.hpp"

namespace {

std::atomic<bool> g_counting{false};
std::atomic<std::uint64_t> g_allocations{0};

void* Allocate(std::size_t size) {
  if (g_counting.load(std::memory_order_relaxed)) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  // NOLINTNEXTLINE(cppcoreguidelines-no-malloc): this is the allocator being replaced.
  void* p = std::malloc(size == 0 ? 1 : size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): as above.
void Free(void* p) noexcept { std::free(p); }

}  // namespace

// The replaceable global allocation functions ([new.delete]); aligned forms are not used here.
void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  try {
    return Allocate(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  try {
    return Allocate(size);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* p) noexcept { Free(p); }
void operator delete[](void* p) noexcept { Free(p); }
void operator delete(void* p, std::size_t /*size*/) noexcept { Free(p); }
void operator delete[](void* p, std::size_t /*size*/) noexcept { Free(p); }

namespace aadp::ad {
namespace {

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261114;
constexpr std::uint64_t kPaths = 4 * pricer::kBlockPaths;

// The counter can fail: a tape that has to grow past its warm-up is seen allocating.
TEST(TapeAllocation, TheCounterSeesATapeGrow) {
  Tape tape;
  const TapeScope scope(tape);
  const Real x = tape.Register(1.0);
  const Position mark = tape.GetPosition();
  static_cast<void>(x * 2.0);
  tape.Rewind(mark);
  g_allocations = 0;
  g_counting = true;
  Real y = x;
  for (std::size_t i = 0; i < 2 * kChunkEntries; ++i) y = y * 1.0001;
  g_counting = false;
  tape.Rewind(mark);
  EXPECT_GE(g_allocations.load(), 1U);
}

TEST(TapeAllocation, NoHeapAllocationAfterWarmUp) {
  std::uint64_t total = 0;
  for (const std::uint32_t steps : {50U, 252U}) {
    for (const testing::Case& c : testing::AllCases(steps)) {
      GradientPathFn path_fn(c.contract, testing::kMarket);
      std::vector<double> normal(steps);
      std::vector<double> crossing(steps);
      const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
      std::array<double, kGradientStats> stats{};
      std::array<double, kGradientStats> sum{};
      // Warm-up: one path and one block end.
      pricer::FillPath(kSeed, 0, normal, crossing);
      path_fn(u, stats);
      path_fn.EndBlock(sum);

      g_allocations = 0;
      g_counting = true;
      for (std::uint64_t p = 1; p <= kPaths; ++p) {
        pricer::FillPath(kSeed, p, normal, crossing);
        path_fn(u, stats);
        for (std::size_t i = 0; i < kGradientStats; ++i) sum[i] += stats[i];
        if (p % pricer::kBlockPaths == 0) path_fn.EndBlock(sum);
        // One allocation already fails the test. A tape that never rewinds also makes every
        // later sweep longer, so stop at the first one rather than run on.
        if (g_allocations.load(std::memory_order_relaxed) != 0) break;
      }
      g_counting = false;
      const std::uint64_t allocations = g_allocations.load();
      total += allocations;
      ASSERT_EQ(allocations, 0U) << c.name << ", " << steps << " steps: allocated after warm-up";
    }
  }
  std::printf(
      "measured: every product and treatment at 50 and 252 steps, %llu paths each after "
      "warm-up: %llu allocations\n",
      static_cast<unsigned long long>(kPaths), static_cast<unsigned long long>(total));
}

}  // namespace
}  // namespace aadp::ad
