// Path-level checks on the kernels. Distribution checks against references live in the plan.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

#include "mc/batch_runner.hpp"
#include "mc/gbm_kernels.hpp"
#include "oracle/finite_difference.hpp"
#include "support/dual.hpp"
#include "support/mc_support.hpp"

namespace aadp {
namespace {

using test::ActiveMarket;
using test::Dual;
using test::ReadGreek;

constexpr MarketParams<double> kMarket{100.0, 0.30, 0.10, 0.02, 0.5};

std::vector<Contract> AllContracts() {
  return {
      {Product::kEuropean, OptionType::kCall, 100.0, 0.0, 1, false},
      {Product::kEuropean, OptionType::kPut, 100.0, 0.0, 1, false},
      {Product::kAsianArithmetic, OptionType::kCall, 100.0, 0.0, 12, true},
      {Product::kAsianArithmetic, OptionType::kPut, 100.0, 0.0, 12, false},
      {Product::kAsianGeometric, OptionType::kCall, 100.0, 0.0, 12, false},
      {Product::kAsianGeometric, OptionType::kPut, 100.0, 0.0, 12, true},
      {Product::kDownOutHard, OptionType::kCall, 100.0, 95.0, 12, false},
      {Product::kDownOutOneStep, OptionType::kCall, 100.0, 95.0, 12, false},
      {Product::kDownOutBridge, OptionType::kCall, 100.0, 95.0, 12, false},
  };
}

std::vector<double> PathUniforms(std::uint64_t path, std::uint32_t steps) {
  std::vector<double> u(steps);
  for (std::uint32_t i = 0; i < steps; ++i) u[i] = PathUniform(99, path, i);
  return u;
}

TEST(Kernels, DualValueIsBitIdenticalToDouble) {
  for (const Contract& c : AllContracts()) {
    for (std::uint64_t path = 0; path < 2000; ++path) {
      const auto u = PathUniforms(path, c.steps);
      EXPECT_EQ(DiscountedPayoff<double>(c, kMarket, u),
                DiscountedPayoff<Dual>(c, ActiveMarket(kMarket), u).value);
    }
  }
}

// On a path away from any kink, jump or barrier the derivative and the difference must agree to
// the difference's round-off, so this is a check of the chain through every kernel.
TEST(Kernels, PathwiseGreeksMatchPathDifferences) {
  constexpr double kH = 1e-6;
  int compared = 0;
  for (const Contract& c : AllContracts()) {
    for (std::uint64_t path = 0; path < 400; ++path) {
      const auto u = PathUniforms(path, c.steps);
      const Dual dual = DiscountedPayoff<Dual>(c, ActiveMarket(kMarket), u);
      if (dual.value == 0.0) continue;
      const double base = std::fabs(dual.value);
      if (c.product == Product::kEuropean || c.product == Product::kAsianArithmetic ||
          c.product == Product::kAsianGeometric) {
        // Skip a path that sits within the bump of the payoff kink.
        if (base < 1e-3) continue;
      }
      for (Greek greek :
           {Greek::kDelta, Greek::kVega, Greek::kRho, Greek::kDividendRho, Greek::kTheta}) {
        const double fd = PathCentralDifference(c, kMarket, greek, kH, u);
        EXPECT_NEAR(ReadGreek(dual, greek), fd, 3e-6 * (1.0 + std::fabs(fd)))
            << static_cast<int>(c.product) << " path " << path << " greek "
            << static_cast<int>(greek);
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 5000);
}

TEST(Kernels, OneStepSurvivalMatchesHardKernelFarFromTheBarrier) {
  Contract hard{Product::kDownOutHard, OptionType::kCall, 100.0, 1e-3, 12, false};
  Contract one_step = hard;
  one_step.product = Product::kDownOutOneStep;
  for (std::uint64_t path = 0; path < 500; ++path) {
    const auto u = PathUniforms(path, 12);
    EXPECT_NEAR(DiscountedPayoff<double>(hard, kMarket, u),
                DiscountedPayoff<double>(one_step, kMarket, u), 1e-9);
  }
}

TEST(Kernels, BridgeWeightIsBelowHardPayoffAndZeroAtTheBarrier) {
  Contract hard{Product::kDownOutHard, OptionType::kCall, 100.0, 95.0, 12, false};
  Contract bridge = hard;
  bridge.product = Product::kDownOutBridge;
  for (std::uint64_t path = 0; path < 500; ++path) {
    const auto u = PathUniforms(path, 12);
    const double weighted = DiscountedPayoff<double>(bridge, kMarket, u);
    EXPECT_GE(weighted, 0.0);
    EXPECT_LE(weighted, DiscountedPayoff<double>(hard, kMarket, u));
  }
  // A spot on the barrier survives no bridge, for any uniforms.
  MarketParams<double> on_barrier = kMarket;
  on_barrier.spot = 95.0;
  EXPECT_EQ(DiscountedPayoff<double>(bridge, on_barrier, PathUniforms(3, 12)), 0.0);
}

// The corner the research flagged: u at its smallest with p close to 1. Everything stays finite,
// because p (1 - u) < 1, and a survival probability of 0 returns 0 rather than -inf.
TEST(Kernels, OneStepSurvivalIsFiniteAtExtremeUniforms) {
  const Contract c{Product::kDownOutOneStep, OptionType::kCall, 100.0, 95.0, 4, false};
  for (const double extreme : {UniformFromBits(0), UniformFromBits(~std::uint64_t{0})}) {
    const std::array<double, 4> u = {extreme, extreme, extreme, extreme};
    const Dual out = DiscountedPayoff<Dual>(c, ActiveMarket(kMarket), u);
    EXPECT_TRUE(std::isfinite(out.value));
    for (double t : out.tangent) EXPECT_TRUE(std::isfinite(t));
  }
  MarketParams<double> far_below = kMarket;
  far_below.spot = 1e-30;
  const std::array<double, 4> u = {0.5, 0.5, 0.5, 0.5};
  EXPECT_EQ(DiscountedPayoff<double>(c, far_below, u), 0.0);
}

TEST(FiniteDifference, RecoversSlopeOfAKnownPayoffWithTheta) {
  // Forward-like: an Asian with one fixing, deep in the money, is linear in spot.
  const Contract c{Product::kAsianArithmetic, OptionType::kCall, 1.0, 0.0, 1, false};
  const std::array<double, 1> u = {0.75};
  const double delta = PathCentralDifference(c, kMarket, Greek::kDelta, 1e-4, u);
  const double value = DiscountedPayoff<double>(c, kMarket, u);
  EXPECT_NEAR(delta, (value + std::exp(-kMarket.r * kMarket.maturity)) / kMarket.spot, 1e-9);
  const double theta = PathCentralDifference(c, kMarket, Greek::kTheta, 1e-5, u);
  const double dvdt = (DiscountedPayoff<double>(c, BumpFor(Greek::kTheta, kMarket, 1e-5), u) -
                       DiscountedPayoff<double>(c, BumpFor(Greek::kTheta, kMarket, -1e-5), u)) /
                      2e-5;
  EXPECT_DOUBLE_EQ(theta, -dvdt);
}

TEST(BumpFor, TouchesOnlyItsParameter) {
  const auto b = BumpFor(Greek::kDividendRho, kMarket, 0.5);
  EXPECT_EQ(b.q, kMarket.q + 0.5);
  EXPECT_EQ(b.spot, kMarket.spot);
  EXPECT_EQ(b.sigma, kMarket.sigma);
  EXPECT_EQ(b.r, kMarket.r);
  EXPECT_EQ(b.maturity, kMarket.maturity);
}

}  // namespace
}  // namespace aadp
