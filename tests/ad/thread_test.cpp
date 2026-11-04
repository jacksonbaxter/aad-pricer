// Threaded gradients (EVIDENCE B10, B11, B14), small enough for the tsan preset (label tsan).
// Each worker of RunBatches builds its own tape and records its own set-up
// (src/ad/gradient.hpp, "Threads"). Checked here:
//   - the value, every Greek, every batch mean and so every SE are the same bits for 1, 2, 3 and
//     8 threads, and equal to a plain loop with no threads that sums paths in path order, sweeps
//     the set-up at each block's end and merges blocks in block order;
//   - the set-up is swept once per block, never per path, whatever the thread count, counted
//     in set-up statements swept by the tapes themselves;
//   - many short paths, so each worker's tape is rewound and reused across many blocks: every
//     Greek's batch means equal the same paths' gradients taken one path at a time, within
//     check 7's bound (tests/ad/oracle_equivalence_test.cpp), restated below;
//   - tapes on separate threads do not see each other.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "ad/gradient.hpp"
#include "ad/tape.hpp"
#include "bounds.hpp"
#include "cases.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::ad {
namespace {

using testing::kMarket;

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261116;
constexpr std::uint32_t kBatches = 4;
// Two blocks and a part block per batch, so workers take blocks out of order.
constexpr std::uint64_t kPaths = kBatches * ((2 * pricer::kBlockPaths) + 5);

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

bool Same(const pricer::Estimate& a, const pricer::Estimate& b) {
  if (Bits(a.mean) != Bits(b.mean) || Bits(a.standard_error) != Bits(b.standard_error)) {
    return false;
  }
  if (a.batch_means.size() != b.batch_means.size()) return false;
  for (std::size_t i = 0; i < a.batch_means.size(); ++i) {
    if (Bits(a.batch_means[i]) != Bits(b.batch_means[i])) return false;
  }
  return true;
}

GradientEstimate RunGradient(const pricer::Contract& c, std::uint64_t paths, std::uint32_t batches,
                             std::uint32_t threads) {
  const auto estimate = PriceWithGradient(
      c, kMarket, {.seed = kSeed, .paths = paths, .batches = batches, .threads = threads});
  EXPECT_TRUE(estimate.has_value());
  return estimate ? *estimate : GradientEstimate{};
}

TEST(TapeThreads, OneTwoThreeAndEightThreadsGiveTheSameBitsForEveryGreek) {
  for (const testing::Case& c : testing::AllCases(8)) {
    const GradientEstimate one = RunGradient(c.contract, kPaths, kBatches, 1);
    ASSERT_EQ(one.value.batch_means.size(), kBatches) << c.name;
    for (const std::uint32_t threads : {2U, 3U, 8U}) {
      const GradientEstimate many = RunGradient(c.contract, kPaths, kBatches, threads);
      EXPECT_TRUE(Same(one.value, many.value)) << c.name << ", value, " << threads << " threads";
      for (std::size_t g = 0; g < kInputs; ++g) {
        EXPECT_TRUE(Same(one.greeks[g], many.greeks[g]))
            << c.name << ", greek " << g << ", " << threads << " threads";
      }
    }
  }
}

// The specification, with no threads and no runner: per block, the paths' payoffs summed in path
// order and the set-up swept once at the block's end (theta as -dV/dT); per batch, the block
// sums in block order, divided by the batch's path count.
std::array<std::vector<double>, kGradientStats> PlainLoop(const pricer::Contract& c) {
  PathGradient path(c, kMarket);
  std::vector<double> normal(c.steps);
  std::vector<double> crossing(pricer::NeedsCrossingUniforms(c) ? c.steps : 0);
  const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
  const std::uint64_t per_batch = kPaths / kBatches;
  std::array<std::vector<double>, kGradientStats> means;
  for (std::uint64_t batch = 0; batch < kBatches; ++batch) {
    std::array<double, kGradientStats> total{};
    for (std::uint64_t first = 0; first < per_batch; first += pricer::kBlockPaths) {
      std::array<double, kGradientStats> block{};
      for (std::uint64_t i = first; i < std::min(first + pricer::kBlockPaths, per_batch); ++i) {
        pricer::FillPath(kSeed, (batch * per_batch) + i, normal, crossing);
        block[0] += path.Accumulate(u);
      }
      const std::array<double, kInputs> gradient = path.SweepSetup();
      for (std::size_t g = 0; g < kInputs; ++g) {
        block[1 + g] += g == kMaturityInput ? -gradient[g] : gradient[g];
      }
      for (std::size_t s = 0; s < kGradientStats; ++s) total[s] += block[s];
    }
    for (std::size_t s = 0; s < kGradientStats; ++s) {
      means[s].push_back(total[s] / static_cast<double>(per_batch));
    }
  }
  return means;
}

bool SameMeans(const pricer::Estimate& a, const std::vector<double>& b) {
  if (a.batch_means.size() != b.size()) return false;
  for (std::size_t i = 0; i < b.size(); ++i) {
    if (Bits(a.batch_means[i]) != Bits(b[i])) return false;
  }
  return true;
}

TEST(TapeThreads, SingleAndThreeThreadRunsAreThePlainLoopBlockByBlock) {
  for (const testing::Case& c : testing::AllCases(8)) {
    const auto plain = PlainLoop(c.contract);
    for (const std::uint32_t threads : {1U, 3U}) {
      const GradientEstimate run = RunGradient(c.contract, kPaths, kBatches, threads);
      EXPECT_TRUE(SameMeans(run.value, plain[0])) << c.name << ", value, " << threads;
      for (std::size_t g = 0; g < kInputs; ++g) {
        EXPECT_TRUE(SameMeans(run.greeks[g], plain[1 + g]))
            << c.name << ", greek " << g << ", " << threads << " threads";
      }
    }
  }
}

// GradientPathFn as PriceWithGradient makes it, reporting on destruction what its tape swept.
class CountingPathFn {
 public:
  struct Totals {
    std::atomic<std::size_t> setup_statements_swept{0};
    std::atomic<std::size_t> setup_size{0};
    std::atomic<std::size_t> workers{0};
  };

  CountingPathFn(const pricer::Contract& c, Totals& totals) : fn_(c, kMarket), totals_(totals) {}
  ~CountingPathFn() {
    totals_.setup_statements_swept.fetch_add(fn_.path().tape().Swept().setup_statements);
    totals_.setup_size.store(fn_.path().mark().statements);
    totals_.workers.fetch_add(1);
  }
  CountingPathFn(const CountingPathFn&) = delete;
  CountingPathFn& operator=(const CountingPathFn&) = delete;
  CountingPathFn(CountingPathFn&&) = delete;
  CountingPathFn& operator=(CountingPathFn&&) = delete;

  void operator()(const pricer::PathUniforms& u, std::array<double, kGradientStats>& stats) {
    fn_(u, stats);
  }
  void EndBlock(std::array<double, kGradientStats>& sum) { fn_.EndBlock(sum); }

 private:
  GradientPathFn fn_;
  Totals& totals_;
};

// Counted, stated before the count ran: the set-up's statements (slot 0, the five inputs and the
// model) are swept once per block, so blocks times the set-up in all, for any thread count. A
// set-up swept per path would make it (paths + blocks) times the set-up.
TEST(TapeThreads, TheSetUpIsSweptOncePerBlockNeverPerPath) {
  const std::uint64_t per_batch = kPaths / kBatches;
  const std::uint64_t blocks =
      kBatches * ((per_batch + pricer::kBlockPaths - 1) / pricer::kBlockPaths);
  for (const testing::Case& c : testing::AllCases(8)) {
    const pricer::PathShape shape{.steps = c.contract.steps,
                                  .crossing = pricer::NeedsCrossingUniforms(c.contract)};
    for (const std::uint32_t threads : {1U, 2U, 3U, 8U}) {
      const pricer::RunConfig config{
          .seed = kSeed, .paths = kPaths, .batches = kBatches, .threads = threads};
      CountingPathFn::Totals totals;
      const auto table = pricer::RunBatches<kGradientStats>(
          config, shape, [&] { return CountingPathFn(c.contract, totals); });
      EXPECT_EQ(totals.workers.load(), threads) << c.name;
      const std::size_t setup = totals.setup_size.load();
      EXPECT_GT(setup, kInputs) << c.name;
      EXPECT_EQ(totals.setup_statements_swept.load(), blocks * setup)
          << c.name << ", " << threads << " threads: set-up statements swept, against " << blocks
          << " blocks of a " << setup << "-statement set-up; per path it would be "
          << (kPaths + blocks) * setup;
      // What was counted is what PriceWithGradient runs.
      const GradientEstimate run = RunGradient(c.contract, kPaths, kBatches, threads);
      for (std::size_t b = 0; b < kBatches; ++b) {
        for (std::size_t s = 0; s < kGradientStats; ++s) {
          const double expected =
              s == 0 ? run.value.batch_means[b] : run.greeks[s - 1].batch_means[b];
          EXPECT_EQ(Bits(table[b][s]), Bits(expected)) << c.name << ", batch " << b;
        }
      }
    }
  }
}

// Many short paths: 64 blocks over 8 workers, each worker's tape rewound after every path and
// its set-up adjoints cleared after every block. Bound, check 7's with the Dual replaced by the
// same paths' gradients one at a time (PathGradient::Evaluate), stated before measuring. A term
// of path i passes at most
//   run   T_i + O_i on the path, P O_i where the set-up adjoints accumulate over the block, the
//         batch's blocks additions and one division;
//   one   T_i + O_i on the path, the batch's n additions and one division;
// with T_i statements and O_i operands of set-up and path, P paths per block and n per batch,
// so m_i = 2 T_i + (P + 2) O_i + n + 2 blocks + 4 covers both, and per batch
//   |run - one| <= (1 / n) sum_i [2 gamma(m_i) A_i + 2 U_i] (1 + 2 gamma(m_i)) + 2^-1072,
// A_i the path's absolute gradient and U_i its underflow term (bounds.hpp). The bound's own
// rounding is covered as in check 7. Also: 8 threads give the 1-thread bits.
TEST(TapeThreads, ManyShortPathsReuseEachTapeAcrossBlocks) {
  using enum pricer::Product;
  using enum pricer::OptionType;
  using enum pricer::Direction;
  using enum pricer::Knock;
  using enum pricer::Monitoring;
  using enum pricer::Treatment;
  constexpr std::uint32_t kShortBatches = 16;
  constexpr std::uint64_t kPerBatch = (3 * pricer::kBlockPaths) + 7;
  constexpr std::uint64_t kShortPaths = kShortBatches * kPerBatch;
  constexpr std::uint64_t kBlocksPerBatch =
      (kPerBatch + pricer::kBlockPaths - 1) / pricer::kBlockPaths;
  const std::vector<testing::Case> cases = {
      {.name = "european call, 1 step", .contract = testing::Plain(kEuropean, kCall, 1)},
      {.name = "digital put, 1 step", .contract = testing::Plain(kDigital, kPut, 1)},
      {.name = "arithmetic Asian put with spot, 2 steps",
       .contract = testing::Plain(kAsianArithmetic, kPut, 2, true)},
      {.name = "down-out one-step, 2 steps",
       .contract = testing::WithBarrier(kDown, kOut, kDiscrete, kOneStep, 2)},
      {.name = "up-in bridge, 1 step",
       .contract = testing::WithBarrier(kUp, kIn, kContinuous, kBridge, 1)},
  };
  double worst = 0.0;
  for (const testing::Case& c : cases) {
    ASSERT_EQ(pricer::Validate(c.contract, kMarket), pricer::ContractError::kNone) << c.name;
    const GradientEstimate run = RunGradient(c.contract, kShortPaths, kShortBatches, 8);
    const GradientEstimate one_thread = RunGradient(c.contract, kShortPaths, kShortBatches, 1);
    EXPECT_TRUE(Same(run.value, one_thread.value)) << c.name;
    for (std::size_t g = 0; g < kInputs; ++g) {
      EXPECT_TRUE(Same(run.greeks[g], one_thread.greeks[g])) << c.name << ", greek " << g;
    }

    PathGradient path(c.contract, kMarket);
    std::vector<double> normal(c.contract.steps);
    std::vector<double> crossing(c.contract.steps);
    const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
    const auto p = static_cast<double>(pricer::kBlockPaths);
    const auto n = static_cast<double>(kPerBatch);
    for (std::uint64_t b = 0; b < kShortBatches; ++b) {
      std::array<double, kInputs> sum{};
      std::array<double, kInputs> bound{};
      for (std::uint64_t i = 0; i < kPerBatch; ++i) {
        pricer::FillPath(kSeed, (b * kPerBatch) + i, normal, crossing);
        const PathGradient::PathResult r = path.Evaluate(u);
        const double m = (2.0 * static_cast<double>(r.statements)) +
                         ((p + 2.0) * static_cast<double>(r.operands)) + n +
                         (2.0 * static_cast<double>(kBlocksPerBatch)) + 4.0;
        const double gamma = testing::Gamma(m);
        for (std::size_t g = 0; g < kInputs; ++g) {
          sum[g] += g == kMaturityInput ? -r.gradient[g] : r.gradient[g];
          bound[g] +=
              ((2.0 * gamma * r.absolute[g]) + (2.0 * testing::UnderflowTerm(r.operand_sums[g]))) *
              (1.0 + (2.0 * gamma));
        }
      }
      for (std::size_t g = 0; g < kInputs; ++g) {
        const double one = sum[g] / n;
        const double t = run.greeks[g].batch_means[b];
        const double limit = ((bound[g] / n) * (1.0 + 0x1p-50)) + testing::kBoundRounding +
                             (0x1p-52 * (std::fabs(t) + std::fabs(one)));
        ASSERT_LE(std::fabs(t - one), limit) << c.name << ", batch " << b << ", greek " << g
                                             << ": run " << t << ", one at a time " << one;
        if (t != one) worst = std::max(worst, std::fabs(t - one) / limit);
      }
    }
  }
  std::printf(
      "measured: many short paths, %llu paths in %llu blocks, 8 threads: worst batch "
      "|run - one path at a time| / bound %.3g\n",
      static_cast<unsigned long long>(kShortPaths),
      static_cast<unsigned long long>(kShortBatches * kBlocksPerBatch), worst);
}

// Four threads, each with its own tape on a different contract, run at once; each gradient is
// the one the same work gives alone on this thread.
TEST(TapeThreads, TapesOnSeparateThreadsAreIndependent) {
  const std::vector<testing::Case> cases = testing::AllCases(12);
  constexpr std::size_t kWorkers = 4;
  constexpr std::uint64_t kPathsEach = 64;
  const auto work = [&](std::size_t which) {
    const testing::Case& c = cases[(7 * which) % cases.size()];
    PathGradient path(c.contract, kMarket);
    std::vector<double> normal(c.contract.steps);
    std::vector<double> crossing(c.contract.steps);
    const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
    for (std::uint64_t p = 0; p < kPathsEach; ++p) {
      pricer::FillPath(kSeed, p, normal, crossing);
      static_cast<void>(path.Accumulate(u));
    }
    return path.SweepSetup();
  };
  std::array<std::array<double, kInputs>, kWorkers> alone{};
  for (std::size_t w = 0; w < kWorkers; ++w) alone[w] = work(w);
  std::array<std::array<double, kInputs>, kWorkers> together{};
  {
    std::vector<std::jthread> threads;
    threads.reserve(kWorkers);
    for (std::size_t w = 0; w < kWorkers; ++w) {
      threads.emplace_back([&, w] { together[w] = work(w); });
    }
  }
  for (std::size_t w = 0; w < kWorkers; ++w) {
    for (std::size_t i = 0; i < kInputs; ++i) {
      EXPECT_EQ(Bits(alone[w][i]), Bits(together[w][i])) << "worker " << w << ", input " << i;
    }
  }
}

}  // namespace
}  // namespace aadp::ad
