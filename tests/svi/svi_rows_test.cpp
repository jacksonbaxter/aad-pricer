// The 45 surface comparisons of stage 8 (docs/NOTES.md, "Stage 8") on 7a's synthetic surfaces, at a
// development seed: the full ladder, the level rule and the paired comparison of the tape against
// D_{h*} on the same paths, with b from the ladder and no ceiling.
//   A, no floors: all 45 resolve and pass, with the paired SE below 0.1 of the tape's own SE.
//   B, floors fire: every resolved row passes; the unresolved count is printed.
// Builds other than release run a few paths and only check that the ladder runs.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "ad/local_vol_gradient.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/runner.hpp"
#include "production.hpp"
#include "svi/synthetic_surface.hpp"

#ifndef AADP_SMOKE_SIZE
#define AADP_SMOKE_SIZE 1
#endif

namespace aadp::oracle_runner {
namespace {

constexpr bool kSmoke = AADP_SMOKE_SIZE != 0;
// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261128;
constexpr std::uint64_t kPaths = kSmoke ? (1U << 10) : (1U << 14);
constexpr std::uint32_t kBatches = 64;
constexpr std::uint32_t kDates = 11;
constexpr double kPairedToTape = 0.1;

// 7a's three products on the terms of its synthetic tape-against-Dual tests: K = 100, H = 95,
// 11 dates, maturing at the last slice, 4 sub-steps.
std::vector<std::pair<std::string, pricer::LocalVolContract>> Contracts() {
  const auto lv = [](const pricer::Contract& c) {
    return pricer::LocalVolContract{
        .contract = c, .maturity = svi::testing::kExpiries[2], .substeps = 4};
  };
  pricer::Contract asian;
  asian.product = pricer::Product::kAsianArithmetic;
  asian.strike = 100.0;
  asian.steps = kDates;
  const auto barrier = [](pricer::Monitoring m, pricer::Treatment t) {
    pricer::Contract c;
    c.product = pricer::Product::kBarrier;
    c.strike = 100.0;
    c.steps = kDates;
    c.barrier.direction = pricer::Direction::kDown;
    c.barrier.knock = pricer::Knock::kOut;
    c.barrier.monitoring = m;
    c.barrier.treatment = t;
    c.barrier.level = 95.0;
    return c;
  };
  return {
      {"AS", lv(asian)},
      {"DB", lv(barrier(pricer::Monitoring::kDiscrete, pricer::Treatment::kOneStep))},
      {"CB", lv(barrier(pricer::Monitoring::kContinuous, pricer::Treatment::kBridge))},
  };
}

struct Tally {
  std::size_t resolved = 0;
  std::size_t unresolved = 0;
  std::size_t passed = 0;
};

Tally RunSurface(const svi::testing::Surface& s, const char* name, bool expect_clean) {
  const double critical = CriticalValue(0.01, 147, kBatches);
  const pricer::RunConfig config{.seed = kSeed, .paths = kPaths, .batches = kBatches, .threads = 4};
  std::vector<std::size_t> inputs(ad::kParametersPerSlice * s.params.size());
  for (std::size_t i = 0; i < inputs.size(); ++i) inputs[i] = i;
  Tally tally;
  for (const auto& [code, contract] : Contracts()) {
    const auto pilot = PilotSurface(contract, s.terms, s.params, config, critical, inputs);
    if (!pilot) {
      ADD_FAILURE() << name << " " << code << ": " << pilot.error();
      continue;
    }
    for (const SurfaceLadder& row : pilot->rows) {
      const std::string id = std::string(name) + "." + code + "." + std::to_string(row.input);
      EXPECT_TRUE(row.refused.empty()) << id << ": " << row.refused;
      EXPECT_EQ(row.outcome.levels.size(), 9U) << id;
      if (kSmoke) continue;
      if (!row.outcome.resolved) {
        ++tally.unresolved;
        EXPECT_FALSE(expect_clean) << id << " is unresolved";
        continue;
      }
      ++tally.resolved;
      const std::vector<double>& tape = pilot->gradient[row.input].batch_means;
      const ToleranceResult r = CheckPaired(tape, row.d_star,
                                            {.b = row.outcome.b,
                                             .se_ceiling = std::numeric_limits<double>::infinity(),
                                             .critical = critical});
      if (r.passed()) ++tally.passed;
      EXPECT_TRUE(r.passed()) << id << ": tape " << r.estimate << " D " << r.reference << " se "
                              << r.standard_error << " b " << row.outcome.b << " l* "
                              << row.outcome.level;
      if (expect_clean) {
        const double tape_se = BatchMeansStatistics(tape).standard_error;
        EXPECT_LT(r.standard_error, kPairedToTape * tape_se)
            << id << ": paired se " << r.standard_error << " tape se " << tape_se;
      }
    }
  }
  return tally;
}

TEST(SviRows, SurfaceAAllResolveAndPass) {
  const Tally t = RunSurface(svi::testing::SurfaceA(), "A", true);
  if (!kSmoke) {
    EXPECT_EQ(t.resolved, 45U);
    EXPECT_EQ(t.passed, 45U);
  }
}

TEST(SviRows, SurfaceBEveryResolvedRowPasses) {
  const Tally t = RunSurface(svi::testing::SurfaceB(), "B", false);
  std::cout << "surface B: resolved " << t.resolved << ", unresolved " << t.unresolved
            << ", passed " << t.passed << '\n';
  if (!kSmoke) {
    EXPECT_EQ(t.passed, t.resolved);
  }
}

}  // namespace
}  // namespace aadp::oracle_runner
