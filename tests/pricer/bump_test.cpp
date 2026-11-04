// Bump and revalue (src/pricer/bump.hpp) against closed forms, at a development seed.
//
// What a difference estimator is checked against. Under common random numbers each bumped
// pricing is an unbiased estimate of the closed-form value at its own market, for the European
// and the geometric Asian on exact GBM steps. So the difference quotient of the estimates is an
// unbiased estimate of the same quotient of closed forms, (CF(x+) - CF(x-)) / (x+ - x-), whatever
// the step: the reference carries the truncation, and the comparison tests the bumping itself
// (which input moves, by how much, the denominator, theta's sign).
//
// Rule, stated before it ran: src/oracle/tolerance.hpp, |xbar - ref| <= t(1 - alpha_F / (2K),
// B - 1) SE + b, alpha_F = 0.01, B = 64, K = kComparisons. b = 2^-40 (|CF+| + |CF-|) / |x+ - x-|:
// each closed form is a few dozen libm operations, each within a few ULP, and 2^-40 is 2^13 ULP
// of its value, so b covers the reference's round-off after the division by the step with room.
// SE ceiling: 1.5 times the tape's SE for the same Greek on the same paths, plus 2^-40. For a
// Lipschitz payoff the per-path quotient tends to the pathwise derivative, so its SE tends to the
// tape's, and 1.5 allows for the kink paths at a finite step. A larger SE means a broken bump.
#include "pricer/bump.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ad/gradient.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/geometric_asian.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::pricer {
namespace {

constexpr std::uint64_t kSeed = 20261104;  // Development seed, not 1729 or 271828.
constexpr std::uint32_t kBatches = 64;
constexpr RunConfig kConfig{
    .seed = kSeed, .paths = kBatches * 1024, .batches = kBatches, .threads = 4};
constexpr Market<double> kMarket{
    .spot = 100.0, .sigma = 0.25, .r = 0.03, .q = 0.01, .maturity = 1.0};
constexpr BumpSteps kSteps = {0.5, 0.005, 0.005, 0.005, 0.01};
constexpr std::array<const char*, kInputCount> kNames = {"delta", "vega", "rho", "dividend rho",
                                                         "theta"};

Contract Plain(Product product, OptionType type, std::uint32_t steps, bool include_spot = false) {
  Contract c;
  c.product = product;
  c.type = type;
  c.strike = 100.0;
  c.steps = steps;
  c.include_spot = include_spot;
  return c;
}

struct Case {
  std::string name;
  Contract contract;
};

std::vector<Case> Cases() {
  return {{.name = "european call, 1 step",
           .contract = Plain(Product::kEuropean, OptionType::kCall, 1)},
          {.name = "european put, 12 steps",
           .contract = Plain(Product::kEuropean, OptionType::kPut, 12)},
          {.name = "geometric Asian call with spot, 12 steps",
           .contract = Plain(Product::kAsianGeometric, OptionType::kCall, 12, true)}};
}

constexpr std::size_t kComparisons = 3 * 2 * kInputCount;

double ClosedForm(const Contract& c, const Market<double>& m) {
  const MarketParams<double> p{
      .spot = m.spot, .sigma = m.sigma, .r = m.r, .q = m.q, .maturity = m.maturity};
  const aadp::OptionType type =
      c.type == OptionType::kCall ? aadp::OptionType::kCall : aadp::OptionType::kPut;
  if (c.product == Product::kAsianGeometric) {
    return GeometricAsianValue(type, c.strike, c.steps, c.include_spot, p);
  }
  return BlackScholes(type, c.strike, p).value;
}

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

TEST(Bump, ValueIsPricesBitForBit) {
  for (const Case& c : Cases()) {
    const auto price = Price(c.contract, kMarket, kConfig);
    ASSERT_TRUE(price.has_value()) << c.name;
    for (const BumpScheme scheme : {BumpScheme::kCentral, BumpScheme::kForward}) {
      const auto bumped = PriceWithBumps(c.contract, kMarket, kConfig, scheme, kSteps);
      ASSERT_TRUE(bumped.has_value()) << c.name;
      ASSERT_EQ(bumped->value.batch_means.size(), price->batch_means.size());
      for (std::size_t b = 0; b < price->batch_means.size(); ++b) {
        EXPECT_EQ(Bits(bumped->value.batch_means[b]), Bits(price->batch_means[b])) << c.name;
      }
      EXPECT_EQ(Bits(bumped->value.standard_error), Bits(price->standard_error)) << c.name;
    }
  }
}

TEST(Bump, CostsTwoNPlusOneCentralAndNPlusOneForwardPricings) {
  const Contract c = Plain(Product::kEuropean, OptionType::kCall, 1);
  const RunConfig small{.seed = kSeed, .paths = 128, .batches = 2, .threads = 1};
  EXPECT_EQ(PriceWithBumps(c, kMarket, small, BumpScheme::kCentral, kSteps)->pricings,
            (2 * kInputCount) + 1);
  EXPECT_EQ(PriceWithBumps(c, kMarket, small, BumpScheme::kForward, kSteps)->pricings,
            kInputCount + 1);
}

TEST(Bump, MatchesTheClosedFormDifferenceQuotient) {
  const double critical = CriticalValue(kFamilyWiseAlpha, kComparisons, kBatches);
  std::size_t compared = 0;
  double worst = 0.0;
  for (const Case& c : Cases()) {
    const auto tape = ad::PriceWithGradient(c.contract, kMarket, kConfig);
    ASSERT_TRUE(tape.has_value()) << c.name;
    for (const BumpScheme scheme : {BumpScheme::kCentral, BumpScheme::kForward}) {
      const auto bumped = PriceWithBumps(c.contract, kMarket, kConfig, scheme, kSteps);
      ASSERT_TRUE(bumped.has_value()) << c.name;
      const char* scheme_name = scheme == BumpScheme::kCentral ? "central" : "forward";
      for (std::size_t i = 0; i < kInputCount; ++i) {
        const auto input = static_cast<Input>(i);
        const Market<double> up = Bumped(kMarket, input, kSteps[i]);
        const Market<double> down =
            scheme == BumpScheme::kCentral ? Bumped(kMarket, input, -kSteps[i]) : kMarket;
        const double cf_up = ClosedForm(c.contract, up);
        const double cf_down = ClosedForm(c.contract, down);
        const double sign = input == Input::kMaturity ? -1.0 : 1.0;
        const double reference = sign * (cf_up - cf_down) / bumped->denominator[i];
        const double b =
            0x1p-40 * (std::fabs(cf_up) + std::fabs(cf_down)) / std::fabs(bumped->denominator[i]);
        const ToleranceLimits limits{.b = b,
                                     .se_ceiling = (1.5 * tape->greeks[i].standard_error) + 0x1p-40,
                                     .critical = critical};
        const Estimate& greek = bumped->greeks[i];
        const ToleranceResult result = ApplyTolerance(
            {.mean = greek.mean, .standard_error = greek.standard_error}, reference, limits);
        EXPECT_TRUE(result.passed())
            << c.name << ", " << scheme_name << ", " << kNames[i] << ": bump " << greek.mean
            << " (SE " << greek.standard_error << "), closed-form quotient " << reference
            << ", bound " << result.bound << ", SE ceiling " << limits.se_ceiling
            << ", failed: " << result.failed_condition();
        worst = std::max(worst, std::fabs(greek.mean - reference) / result.bound);
        ++compared;
      }
    }
  }
  EXPECT_EQ(compared, kComparisons);
  std::printf(
      "measured: %zu bump Greeks against closed-form quotients, worst distance / bound "
      "%.3f, critical value %.4f\n",
      compared, worst, critical);
}

TEST(Bump, HoldsTheSmoothingHalfWidthInCurrency) {
  Contract c = Plain(Product::kBarrier, OptionType::kCall, 4);
  c.barrier = {.direction = Direction::kUp,
               .knock = Knock::kOut,
               .monitoring = Monitoring::kDiscrete,
               .treatment = Treatment::kSmoothed,
               .level = 150.0,
               .smoothing_width = 0.05};
  const double half_width = c.barrier.smoothing_width * kMarket.spot;
  for (const double h : {kSteps[0], -kSteps[0], 0x1p-20, 3.0}) {
    const Market<double> up = Bumped(kMarket, Input::kSpot, h);
    const double held = ContractAt(c, kMarket, up).barrier.smoothing_width * up.spot;
    EXPECT_LE(std::fabs(held - half_width), 2.0 * (std::nextafter(half_width, 1e300) - half_width))
        << h;
  }
  // Other inputs and other products keep their terms.
  const Market<double> vol = Bumped(kMarket, Input::kSigma, 0.01);
  EXPECT_EQ(ContractAt(c, kMarket, vol).barrier.smoothing_width, c.barrier.smoothing_width);
  const Contract european = Plain(Product::kEuropean, OptionType::kCall, 1);
  const Market<double> spot = Bumped(kMarket, Input::kSpot, 1.0);
  EXPECT_EQ(ContractAt(european, kMarket, spot).barrier.smoothing_width, 0.0);
}

TEST(Bump, RefusesABumpedMarketAndNamesTheInput) {
  Contract c = Plain(Product::kBarrier, OptionType::kCall, 4);
  c.barrier = {.direction = Direction::kDown,
               .knock = Knock::kOut,
               .monitoring = Monitoring::kDiscrete,
               .treatment = Treatment::kOneStep,
               .level = 99.9,
               .smoothing_width = 0.0};
  const RunConfig small{.seed = kSeed, .paths = 128, .batches = 2, .threads = 1};
  const auto refused = PriceWithBumps(c, kMarket, small, BumpScheme::kCentral, kSteps);
  ASSERT_FALSE(refused.has_value());
  EXPECT_TRUE(refused.error().bumped);
  EXPECT_EQ(refused.error().input, Input::kSpot);
  EXPECT_EQ(refused.error().price.contract, ContractError::kBarrierLevel);
  // The forward bump moves the spot away from the barrier, so it prices.
  EXPECT_TRUE(PriceWithBumps(c, kMarket, small, BumpScheme::kForward, kSteps).has_value());
}

}  // namespace
}  // namespace aadp::pricer
