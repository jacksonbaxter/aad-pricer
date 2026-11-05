// The parity regression, the TX lookup, time to expiry, the tick table and the input butterfly
// count on hand-made numbers. The real-file oracles are in realdata_test.cpp.
#include "data/forwards.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "data/calibration_set.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"

namespace aadp {
namespace {

using taifex::OptionRight;
using taifex::OptionRow;
using taifex::Session;

OptionRow Row(double strike, OptionRight right, double settlement,
              Session session = Session::kRegular) {
  OptionRow row;
  row.contract = "TXO";
  row.expiry = "202411";
  row.strike = strike;
  row.right = right;
  row.settlement_price = settlement;
  row.session = session;
  return row;
}

TEST(SettlementParityPoints, PairsCallAndPutPerStrikeInTheRegularSessionOnly) {
  std::vector<OptionRow> rows = {Row(100, OptionRight::kCall, 7.0),
                                 Row(100, OptionRight::kPut, 2.0),
                                 Row(90, OptionRight::kPut, 1.0),
                                 Row(90, OptionRight::kCall, 12.0),
                                 Row(110, OptionRight::kCall, 3.0),  // no put
                                 Row(120, OptionRight::kCall, 1.0, Session::kAfterHours),
                                 Row(120, OptionRight::kPut, 20.0, Session::kAfterHours)};
  OptionRow other_expiry = Row(95, OptionRight::kCall, 9.0);
  other_expiry.expiry = "202412";
  rows.push_back(other_expiry);
  OptionRow other_contract = Row(95, OptionRight::kPut, 4.0);
  other_contract.contract = "TEO";
  rows.push_back(other_contract);
  OptionRow unsettled = Row(130, OptionRight::kCall, 0.0);
  unsettled.settlement_price.reset();
  rows.push_back(unsettled);
  rows.push_back(Row(130, OptionRight::kPut, 25.0));

  const std::vector<ParityPoint> points = settlement_parity_points(rows, "202411");
  ASSERT_EQ(points.size(), 2U);
  EXPECT_EQ(points[0].strike, 90.0);
  EXPECT_EQ(points[0].call_minus_put, 11.0);
  EXPECT_EQ(points[1].strike, 100.0);
  EXPECT_EQ(points[1].call_minus_put, 5.0);
}

TEST(FitParity, RecoversForwardAndDiscountFromAnExactLine) {
  // C - P = D (F - K) with F = 22800, D = 0.999, on strikes 20000 to 25000.
  std::vector<ParityPoint> points;
  for (int i = 0; i <= 50; ++i) {
    const double k = 20000.0 + (100.0 * i);
    points.push_back({.strike = k, .call_minus_put = 0.999 * (22800.0 - k)});
  }
  const auto fit = fit_parity(points);
  ASSERT_TRUE(fit.has_value());
  EXPECT_EQ(fit->points, points.size());
  EXPECT_NEAR(fit->forward, 22800.0, 1e-8);
  EXPECT_NEAR(fit->discount, 0.999, 1e-14);
  EXPECT_LT(fit->rms_residual, 1e-9);
  EXPECT_LT(fit->max_abs_residual, 1e-9);
}

TEST(FitParity, ResidualStatisticsByHand) {
  // Strikes -1, 0, 1 and differences 2 + e, -e... chosen so the line is y = 1 - x exactly with
  // residuals (+0.5, -1, +0.5): sum 0, orthogonal to x.
  const std::vector<ParityPoint> points = {{.strike = -1.0, .call_minus_put = 2.5},
                                           {.strike = 0.0, .call_minus_put = 0.0},
                                           {.strike = 1.0, .call_minus_put = 0.5}};
  const auto fit = fit_parity(points);
  ASSERT_TRUE(fit.has_value());
  EXPECT_DOUBLE_EQ(fit->discount, 1.0);
  EXPECT_DOUBLE_EQ(fit->forward, 1.0);
  // Sum of squares 1.5: RMS over n = sqrt(0.5), sigma_leg = 0.5, max 1.
  EXPECT_DOUBLE_EQ(fit->rms_residual, std::sqrt(0.5));
  EXPECT_DOUBLE_EQ(fit->sigma_leg, 0.5);
  EXPECT_DOUBLE_EQ(fit->max_abs_residual, 1.0);
  // s2 = 1.5 / (n - 2) = 1.5, sxx = 2: var(slope) = 0.75, var(intercept) = 0.5, cov = 0,
  // var(F) = (0.5 + 0.75) / 1.
  EXPECT_DOUBLE_EQ(fit->discount_se, std::sqrt(0.75));
  EXPECT_DOUBLE_EQ(fit->forward_se, std::sqrt(1.25));
}

TEST(FitParity, ErrorsAreValues) {
  EXPECT_EQ(fit_parity(std::vector<ParityPoint>{{.strike = 1, .call_minus_put = 1},
                                                {.strike = 2, .call_minus_put = 0}})
                .error(),
            ParityError::kTooFewPoints);
  EXPECT_EQ(fit_parity(std::vector<ParityPoint>{{.strike = 1, .call_minus_put = 1},
                                                {.strike = 1, .call_minus_put = 0},
                                                {.strike = 1, .call_minus_put = 2}})
                .error(),
            ParityError::kNoStrikeSpread);
  EXPECT_EQ(fit_parity(std::vector<ParityPoint>{{.strike = 1, .call_minus_put = 1},
                                                {.strike = 2, .call_minus_put = 2},
                                                {.strike = 3, .call_minus_put = 3}})
                .error(),
            ParityError::kNonPositiveDiscount);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(fit_parity(std::vector<ParityPoint>{{.strike = 1, .call_minus_put = 1},
                                                {.strike = 2, .call_minus_put = nan},
                                                {.strike = 3, .call_minus_put = 0}})
                .error(),
            ParityError::kNonFinite);
}

TEST(TxSettlement, RegularOutrightOnly) {
  std::vector<taifex::FuturesRow> rows(4);
  for (auto& row : rows) {
    row.contract = "TX";
    row.expiry = "202411";
  }
  rows[0].settlement_price = 1.0;
  rows[0].session = Session::kAfterHours;
  rows[1].settlement_price = 2.0;
  rows[1].calendar_spread = true;
  rows[2].contract = "MTX";
  rows[2].settlement_price = 3.0;
  rows[3].settlement_price = 4.0;
  EXPECT_EQ(tx_settlement(rows, "202411"), 4.0);
  EXPECT_FALSE(tx_settlement(rows, "202411W4").has_value());
}

TEST(TimeToExpiry, CalendarSecondsFromTheCloseToTheLastDayClose) {
  // Five, twelve and thirty-three days less fifteen minutes.
  EXPECT_EQ(txo_seconds_to_expiry("202411"), (5 * 86400) - 900);
  EXPECT_EQ(txo_seconds_to_expiry("202411W4"), (12 * 86400) - 900);
  EXPECT_EQ(txo_seconds_to_expiry("202412"), (33 * 86400) - 900);
  EXPECT_EQ(txo_seconds_to_expiry("202501"), (61 * 86400) - 900);
  EXPECT_EQ(txo_seconds_to_expiry("202503"), (124 * 86400) - 900);
  EXPECT_EQ(txo_seconds_to_expiry("202506"), (215 * 86400) - 900);
  EXPECT_FALSE(txo_seconds_to_expiry("202502").has_value());
  EXPECT_DOUBLE_EQ(txo_years_to_expiry("202411").value_or(0.0), 431100.0 / (365.0 * 86400.0));
  // Phase 1's profile printed T to four places: 0.0137, 0.0328, 0.0904.
  EXPECT_NEAR(txo_years_to_expiry("202411").value_or(0.0), 0.0137, 5e-5);
  EXPECT_NEAR(txo_years_to_expiry("202411W4").value_or(0.0), 0.0328, 5e-5);
  EXPECT_NEAR(txo_years_to_expiry("202412").value_or(0.0), 0.0904, 5e-5);
}

TEST(SettlementTick, RulesArticleSeven) {
  EXPECT_EQ(settlement_tick(0.1), 0.1);
  EXPECT_EQ(settlement_tick(9.9), 0.1);
  EXPECT_EQ(settlement_tick(10.0), 0.5);
  EXPECT_EQ(settlement_tick(49.5), 0.5);
  EXPECT_EQ(settlement_tick(50.0), 1.0);
  EXPECT_EQ(settlement_tick(499.0), 1.0);
  EXPECT_EQ(settlement_tick(500.0), 5.0);
  EXPECT_EQ(settlement_tick(995.0), 5.0);
  EXPECT_EQ(settlement_tick(1000.0), 10.0);
}

TEST(CountButterflies, ConvexityAndTheHalfTickBand) {
  using R = OptionRight;
  // Calls at 100, 110, 120 priced 10, 5, 1: excess 5 - 5.5 < 0, convex.
  const std::vector<StrikePrice> convex = {{.strike = 100, .price = 10, .right = R::kCall},
                                           {.strike = 110, .price = 5, .right = R::kCall},
                                           {.strike = 120, .price = 1, .right = R::kCall}};
  const ButterflyCounts c1 = count_butterflies(convex);
  EXPECT_EQ(c1.triples, 1U);
  EXPECT_EQ(c1.non_convex, 0U);

  // Puts, given out of order, at 90, 95, 100 priced 1, 2.04, 3: w = 1/2, excess 0.04, so
  // non-convex. Every price is below 10, tick 0.1, so the band is 0.025 + 0.05 + 0.025 = 0.1 and
  // the excess is inside it.
  const std::vector<StrikePrice> inside = {{.strike = 100, .price = 3, .right = R::kPut},
                                           {.strike = 90, .price = 1, .right = R::kPut},
                                           {.strike = 95, .price = 2.04, .right = R::kPut}};
  const ButterflyCounts c2 = count_butterflies(inside);
  EXPECT_EQ(c2.triples, 1U);
  EXPECT_EQ(c2.non_convex, 1U);
  EXPECT_EQ(c2.beyond_half_tick, 0U);

  // Excess 0.5 at prices that straddle the tick levels: 10 (tick 0.5), 7, 3 (tick 0.1): band
  // 0.5 * 0.25 + 0.05 + 0.5 * 0.05 = 0.2, so beyond.
  const std::vector<StrikePrice> beyond = {{.strike = 100, .price = 10, .right = R::kCall},
                                           {.strike = 110, .price = 7, .right = R::kCall},
                                           {.strike = 120, .price = 3, .right = R::kCall},
                                           {.strike = 80, .price = 1, .right = R::kPut}};
  const ButterflyCounts c3 = count_butterflies(beyond);
  EXPECT_EQ(c3.triples, 1U);
  EXPECT_EQ(c3.non_convex, 1U);
  EXPECT_EQ(c3.beyond_half_tick, 1U);
}

TEST(CountButterflies, CallsAndPutsNeverShareATriple) {
  using R = OptionRight;
  const std::vector<StrikePrice> mixed = {{.strike = 90, .price = 1, .right = R::kPut},
                                          {.strike = 95, .price = 9, .right = R::kPut},
                                          {.strike = 105, .price = 9, .right = R::kCall},
                                          {.strike = 110, .price = 1, .right = R::kCall}};
  EXPECT_EQ(count_butterflies(mixed).triples, 0U);
}

}  // namespace
}  // namespace aadp
