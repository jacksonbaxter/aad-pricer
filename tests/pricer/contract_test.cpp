// Validation, draws and run configuration of the pricer entry point.
#include "pricer/contract.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

#include "pricer/draws.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "rng/philox.hpp"

namespace aadp::pricer {
namespace {

constexpr Market<double> kMarket{.spot = 100.0, .sigma = 0.3, .r = 0.05, .q = 0.01, .maturity = 1};

Contract European() {
  Contract c;
  c.strike = 100.0;
  return c;
}

Contract Barrier(Monitoring monitoring, Treatment treatment, double level = 90.0) {
  return {.product = Product::kBarrier,
          .type = OptionType::kCall,
          .strike = 100.0,
          .steps = 12,
          .include_spot = false,
          .barrier = {.direction = Direction::kDown,
                      .knock = Knock::kOut,
                      .monitoring = monitoring,
                      .treatment = treatment,
                      .level = level,
                      .smoothing_width = 0.01}};
}

TEST(PricerDraws, NormalUniformIsTheReferenceStream) {
  for (const std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{20261104}, ~std::uint64_t{0}}) {
    for (const std::uint64_t path : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{1} << 40}) {
      for (const std::uint32_t step : {0U, 1U, 49U, 0xffffffffU}) {
        const StepUniforms u = DrawStep(seed, path, step);
        EXPECT_EQ(u.normal, PathUniform(seed, path, step));
        EXPECT_GT(u.crossing, 0.0);
        EXPECT_LT(u.crossing, 1.0);
        EXPECT_NE(u.crossing, u.normal);
      }
    }
  }
}

TEST(PricerValidate, AcceptsEveryTreatmentOnItsOwnMonitoring) {
  EXPECT_EQ(Validate(Barrier(Monitoring::kContinuous, Treatment::kBridge), kMarket),
            ContractError::kNone);
  EXPECT_EQ(Validate(Barrier(Monitoring::kContinuous, Treatment::kHard), kMarket),
            ContractError::kNone);
  EXPECT_EQ(Validate(Barrier(Monitoring::kDiscrete, Treatment::kOneStep), kMarket),
            ContractError::kNone);
  EXPECT_EQ(Validate(Barrier(Monitoring::kDiscrete, Treatment::kHard), kMarket),
            ContractError::kNone);
  EXPECT_EQ(Validate(Barrier(Monitoring::kDiscrete, Treatment::kSmoothed), kMarket),
            ContractError::kNone);
}

TEST(PricerValidate, RefusesTheBridgeOnADiscreteBarrier) {
  EXPECT_EQ(Validate(Barrier(Monitoring::kDiscrete, Treatment::kBridge), kMarket),
            ContractError::kBridgeOnDiscrete);
  EXPECT_EQ(Validate(Barrier(Monitoring::kContinuous, Treatment::kOneStep), kMarket),
            ContractError::kOneStepOnContinuous);
  EXPECT_EQ(Validate(Barrier(Monitoring::kContinuous, Treatment::kSmoothed), kMarket),
            ContractError::kSmoothedOnContinuous);
}

TEST(PricerValidate, RefusesABarrierOnTheWrongSide) {
  EXPECT_EQ(Validate(Barrier(Monitoring::kDiscrete, Treatment::kHard, 100.0), kMarket),
            ContractError::kBarrierLevel);
  Contract up = Barrier(Monitoring::kDiscrete, Treatment::kHard, 90.0);
  up.barrier.direction = Direction::kUp;
  EXPECT_EQ(Validate(up, kMarket), ContractError::kBarrierLevel);
  up.barrier.level = 110.0;
  EXPECT_EQ(Validate(up, kMarket), ContractError::kNone);
}

TEST(PricerValidate, RefusesEveryBadField) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const Contract european = European();
  EXPECT_EQ(Validate(european, kMarket), ContractError::kNone);

  Contract c = european;
  c.product = static_cast<Product>(kProductCount);
  EXPECT_EQ(Validate(c, kMarket), ContractError::kProduct);
  c = european;
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): the value it must refuse.
  c.type = static_cast<OptionType>(2);
  EXPECT_EQ(Validate(c, kMarket), ContractError::kOptionType);
  c = european;
  c.strike = nan;
  EXPECT_EQ(Validate(c, kMarket), ContractError::kStrike);
  c = european;
  c.steps = 0;
  EXPECT_EQ(Validate(c, kMarket), ContractError::kSteps);
  c.steps = kMaxSteps + 1;
  EXPECT_EQ(Validate(c, kMarket), ContractError::kSteps);
  c = european;
  c.include_spot = true;
  EXPECT_EQ(Validate(c, kMarket), ContractError::kIncludeSpot);

  Market<double> m = kMarket;
  m.spot = 0.0;
  EXPECT_EQ(Validate(european, m), ContractError::kSpot);
  m = kMarket;
  m.sigma = nan;
  EXPECT_EQ(Validate(european, m), ContractError::kVolatility);
  m = kMarket;
  m.maturity = -1.0;
  EXPECT_EQ(Validate(european, m), ContractError::kMaturity);
  m = kMarket;
  m.q = std::numeric_limits<double>::infinity();
  EXPECT_EQ(Validate(european, m), ContractError::kRate);

  Contract b = Barrier(Monitoring::kDiscrete, Treatment::kSmoothed);
  b.barrier.smoothing_width = 0.0;
  EXPECT_EQ(Validate(b, kMarket), ContractError::kSmoothingWidth);
  b.barrier.treatment = static_cast<Treatment>(kTreatmentCount);
  EXPECT_EQ(Validate(b, kMarket), ContractError::kBarrierField);
}

// sigma = 10 over 100 years puts sigma^2 T / 2 at 5000, far past exp's range.
TEST(PricerValidate, RefusesParametersThatCouldLeaveTheFiniteRange) {
  const Contract european = European();
  Market<double> m = kMarket;
  m.sigma = kMaxVolatility;
  m.maturity = kMaxMaturity;
  EXPECT_EQ(Validate(european, m), ContractError::kRange);
  m.maturity = 1.0;
  EXPECT_EQ(Validate(european, m), ContractError::kNone);
}

TEST(PricerValidate, EveryErrorHasAName) {
  for (int e = 0; e <= static_cast<int>(ContractError::kRange); ++e) {
    EXPECT_NE(std::string_view(ErrorName(static_cast<ContractError>(e))), "unknown error") << e;
  }
}

TEST(PricerRun, RefusesABadRunConfiguration) {
  const Contract european = European();
  EXPECT_EQ(Price(european, kMarket, {.seed = 3, .paths = 64, .batches = 1}).error().run,
            RunError::kBatches);
  EXPECT_EQ(Price(european, kMarket, {.seed = 3, .paths = 65, .batches = 2}).error().run,
            RunError::kPaths);
  EXPECT_EQ(
      Price(european, kMarket, {.seed = 3, .paths = 64, .batches = 2, .threads = 0}).error().run,
      RunError::kThreads);
  Contract bad = european;
  bad.steps = 0;
  EXPECT_EQ(Price(bad, kMarket, {.seed = 3, .paths = 64, .batches = 2}).error().contract,
            ContractError::kSteps);
  const auto good = Price(european, kMarket, {.seed = 3, .paths = 64, .batches = 2});
  ASSERT_TRUE(good.has_value());
  EXPECT_EQ(good->batch_means.size(), 2U);
  EXPECT_TRUE(std::isfinite(good->mean));
  EXPECT_TRUE(std::isfinite(good->standard_error));
}

}  // namespace
}  // namespace aadp::pricer
