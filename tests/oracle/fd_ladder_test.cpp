// The ladder rule of the 45 surface rows (NOTES.md, "Stage 8") on hand-made batch tables whose
// answer is known, the first step h0 on a slice whose maximum slope is known in closed form, the
// domain refusal, and a bump that moves g through 0 on a path-step and so must change the tally.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "ladder.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/runner.hpp"
#include "production.hpp"
#include "runner.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/synthetic_surface.hpp"

namespace aadp::oracle_runner {
namespace {

constexpr std::size_t kBatches = 64;
// The plan's critical value: K = 147, alpha_F = 0.01, B = 64.
const double kCritical = CriticalValue(0.01, 147, kBatches);
constexpr double kH0 = 0.01;

using Table = std::array<std::vector<double>, kLadderLevels>;

std::array<double, kLadderLevels> Steps() {
  std::array<double, kLadderLevels> h{};
  for (std::size_t l = 0; l < kLadderLevels; ++l) h[l] = std::ldexp(kH0, -static_cast<int>(l));
  return h;
}

// D_{l,q} = truncation(h_l) + noise, with independent noise of size `noise` per level and batch
// from a fixed generator (mt19937_64's output is the same on every library).
template <class Truncation>
Table MakeTable(Truncation truncation, double noise, std::uint64_t seed = 20261128) {
  std::mt19937_64 bits(seed);
  const auto h = Steps();
  Table d;
  for (std::size_t l = 0; l < kLadderLevels; ++l) {
    d[l].resize(kBatches);
    for (double& x : d[l]) {
      const double u = (static_cast<double>(bits() >> 11) * 0x1p-53) - 0.5;
      x = 1.0 + truncation(h[l]) + (noise * u);
    }
  }
  return d;
}

// b by the formula, from the table itself.
double ExpectedB(const Table& d, std::size_t level) {
  std::vector<double> delta(kBatches);
  for (std::size_t q = 0; q < kBatches; ++q) delta[q] = d[level][q] - d[level + 1][q];
  const BatchStatistics s = BatchMeansStatistics(delta);
  return RoundUpToThreeFigures(2.0 * (std::fabs(s.mean) + (kCritical * s.standard_error)) / 3.0);
}

TEST(LadderRule, PureSquareTruncationWithNoiseChoosesTheFirstLevel) {
  const Table d = MakeTable([](double h) { return 40.0 * h * h; }, 1e-9);
  const LadderOutcome o = ApplyLadderRule(Steps(), d, kCritical);
  ASSERT_TRUE(o.resolved);
  EXPECT_EQ(o.level, 0U);
  EXPECT_EQ(o.h_star, Steps()[1]);
  EXPECT_EQ(o.b, ExpectedB(d, 0));
  // b covers the truncation at h_1, A h_1^2 = 40 h0^2 / 4, within the rounding of b.
  EXPECT_GE(o.b, 40.0 * Steps()[1] * Steps()[1] * 2.0 * 0.999);
}

TEST(LadderRule, FirstOrderTruncationIsUnresolved) {
  const Table d = MakeTable([](double h) { return 3.0 * h; }, 0.0);
  const LadderOutcome o = ApplyLadderRule(Steps(), d, kCritical);
  EXPECT_FALSE(o.resolved);
  EXPECT_EQ(o.h_star, Steps()[8]);
  for (std::size_t l = 0; l + 2 < kLadderLevels; ++l) EXPECT_FALSE(o.levels[l].consistent) << l;
}

TEST(LadderRule, LateSettlingChoosesTheFirstLevelWithTwoConsistentPairs) {
  // D = A h^2 + B h^4 with B h0^2 = 10 A: a pair is consistent once B h^2 <= 0.4 A, from l = 3.
  const double b4 = 10.0 / (kH0 * kH0);
  const Table d = MakeTable([&](double h) { return (h * h) + (b4 * h * h * h * h); }, 0.0);
  const LadderOutcome o = ApplyLadderRule(Steps(), d, kCritical);
  ASSERT_TRUE(o.resolved);
  EXPECT_FALSE(o.levels[2].consistent);
  EXPECT_TRUE(o.levels[3].consistent);
  EXPECT_TRUE(o.levels[4].consistent);
  EXPECT_EQ(o.level, 3U);
  EXPECT_EQ(o.h_star, Steps()[4]);
  EXPECT_EQ(o.b, ExpectedB(d, 3));
}

TEST(LadderRule, OneConsistentPairWhereTheQuarticTermCrossesZeroIsNotChosen) {
  // D = A h^2 + B h^4 + C h^6 with C h0^2 = -(16 / 21) B: E_0 = -(45/256) B h0^4 - (945/4096) C
  // h0^6 = 0, so pair 0 is consistent by a cancellation, pair 1 is not, and pairs 3 and 4 are.
  const double b4 = 21.0 / (kH0 * kH0);
  const double c6 = -16.0 / (kH0 * kH0 * kH0 * kH0);
  const Table d = MakeTable(
      [&](double h) {
        const double h2 = h * h;
        return h2 + (b4 * h2 * h2) + (c6 * h2 * h2 * h2);
      },
      0.0);
  const LadderOutcome o = ApplyLadderRule(Steps(), d, kCritical);
  EXPECT_TRUE(o.levels[0].consistent);
  EXPECT_FALSE(o.levels[1].consistent);
  EXPECT_FALSE(o.levels[2].consistent);
  ASSERT_TRUE(o.resolved);
  EXPECT_EQ(o.level, 3U);
  EXPECT_EQ(o.h_star, Steps()[4]);
}

TEST(LadderRule, NoiseOnlyChoosesTheFirstLevelWithBFromTheStandardError) {
  const Table d = MakeTable([](double) { return 0.0; }, 1e-3);
  const LadderOutcome o = ApplyLadderRule(Steps(), d, kCritical);
  ASSERT_TRUE(o.resolved);
  EXPECT_EQ(o.level, 0U);
  EXPECT_EQ(o.b, ExpectedB(d, 0));
  // Most of b is the c SE term: the mean of pure noise is well inside it.
  EXPECT_GT(o.b, 2.0 * kCritical * o.two_h.standard_error / 3.0);
}

TEST(FirstStep, ClosedFormMaxima) {
  const svi::RawSvi<double> p{.a = 0.001, .b = 0.05, .rho = -0.4, .m = 0.3, .sigma = 0.1};
  const double w0 = p.a + (p.b * ((-p.rho * p.m) + std::sqrt((p.m * p.m) + (p.sigma * p.sigma))));
  EXPECT_DOUBLE_EQ(AtTheMoneyTotalVariance(p), w0);
  // a: the slope is 1 everywhere.
  EXPECT_DOUBLE_EQ(FirstStep(p, 0), 0x1p-8 * w0);
  // rho: |b (k - m)| is largest at k = -0.5, b 0.8, not anywhere in a narrower quoted range.
  EXPECT_DOUBLE_EQ(FirstStep(p, 2), 0x1p-8 * w0 / (p.b * 0.8));
  // sigma: b sigma / r is largest at the vertex k = m, where it is b.
  EXPECT_DOUBLE_EQ(FirstStep(p, 4), 0x1p-8 * w0 / p.b);
  // b: rho (k - m) + r is largest at k = -0.5 here: 0.32 + sqrt(0.64 + 0.01).
  EXPECT_DOUBLE_EQ(FirstStep(p, 1), 0x1p-8 * w0 / (0.32 + std::sqrt(0.65)));
}

TEST(FirstStep, RefusesAStepOutsideTheDomain) {
  const svi::RawSvi<double> p{.a = 0.001, .b = 0.05, .rho = 0.99999, .m = 0.0, .sigma = 0.1};
  EXPECT_TRUE(StepOutsideDomain(p, 2, 1e-4).has_value());
  EXPECT_FALSE(StepOutsideDomain(p, 2, 1e-6).has_value());
  EXPECT_TRUE(StepOutsideDomain(p, 4, 0.1).has_value());
  EXPECT_TRUE(StepOutsideDomain(p, 1, 0.06).has_value());
  EXPECT_FALSE(StepOutsideDomain(p, 3, 10.0).has_value());
}

std::uint64_t ButterflyCount(const pricer::FloorTally& t) {
  return t.by_class[static_cast<std::size_t>(svi::LocalVarianceClass::kButterfly)];
}

TEST(Tally, ABumpThatMovesGThroughZeroChangesTheButterflyCount) {
  // Surface A has no floored point. Lowering slice 1's a by 0.001 takes g below 0 on path-steps
  // the base leaves alone (52 of them on these paths, found by trying steps of 1e-4 to 3e-2 on all
  // 15 parameters), so the minus side's butterfly count must change.
  const svi::testing::Surface s = svi::testing::SurfaceA();
  pricer::Contract c;
  c.product = pricer::Product::kEuropean;
  c.strike = 100.0;
  c.steps = 11;
  const pricer::LocalVolContract lv{
      .contract = c, .maturity = svi::testing::kExpiries[2], .substeps = 4};
  const pricer::RunConfig config{.seed = 20261128, .paths = 4096, .batches = 64, .threads = 1};
  const auto base = pricer::PriceLocalVol(lv, s.terms, s.params, config);
  ASSERT_TRUE(base.has_value());
  ASSERT_EQ(ButterflyCount(base->floors), 0U);
  constexpr std::size_t kA1 = 5;
  const double h = 0.001;
  auto down = s.params;
  down[1].a -= h;
  const auto minus = pricer::PriceLocalVol(lv, s.terms, down, config);
  ASSERT_TRUE(minus.has_value());
  ASSERT_GT(ButterflyCount(minus->floors), 0U) << "the constructed bump no longer floors";
  const auto d = SurfaceCentralDifference(lv, s.terms, s.params, kA1, h, config, base->floors);
  ASSERT_TRUE(d.has_value()) << d.error();
  EXPECT_EQ(d->tally.butterfly_minus, static_cast<std::int64_t>(ButterflyCount(minus->floors)));
  EXPECT_NE(d->tally.butterfly_minus, 0);
}

}  // namespace
}  // namespace aadp::oracle_runner
