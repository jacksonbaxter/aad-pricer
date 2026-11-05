// The chain into the fit (src/svi/chain_fit.hpp) on synthetic rows from hand-made parameters.
#include "svi/chain_fit.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <vector>

#include "data/calibration_set.hpp"
#include "data/forwards.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
#include "svi/arbitrage.hpp"
#include "svi/fit.hpp"
#include "svi/implied_vol.hpp"
#include "synthetic_chain.hpp"

namespace aadp::svi {
namespace {

using testing::SyntheticExpiry;

const std::vector<SyntheticExpiry> kSurface = {
    {.expiry = "202411",
     .forward = 22800.0,
     .discount = 0.9999,
     .params = {.a = 1e-4, .b = 0.012, .rho = -0.45, .m = 0.002, .sigma = 0.025}},
    {.expiry = "202411W4",
     .forward = 22810.0,
     .discount = 0.9995,
     .params = {.a = 2e-4, .b = 0.02, .rho = -0.35, .m = 0.005, .sigma = 0.04}},
    {.expiry = "202412",
     .forward = 22830.0,
     .discount = 0.998,
     .params = {.a = 5e-4, .b = 0.04, .rho = -0.5, .m = 0.01, .sigma = 0.06}}};

constexpr std::array<std::string_view, 3> kExpiries = {"202411", "202411W4", "202412"};

TEST(BuildChainSlices, ForwardsTimesQuotesAndWeights) {
  const auto rows = testing::SyntheticRows(kSurface, 20000.0, 25000.0, 100.0);
  const auto slices = BuildChainSlices(rows, {}, kExpiries);
  ASSERT_TRUE(slices.has_value()) << ChainErrorName(slices.error().code);
  ASSERT_EQ(slices->size(), 3U);
  for (std::size_t i = 0; i < 3; ++i) {
    const ChainSlice& s = (*slices)[i];
    EXPECT_EQ(s.expiry, kSurface[i].expiry);
    EXPECT_EQ(s.years, txo_years_to_expiry(s.expiry).value_or(0.0));
    // Prices rounded to 0.1 move F by far less than a point.
    EXPECT_NEAR(s.parity.forward, kSurface[i].forward, 0.5) << s.expiry;
    EXPECT_NEAR(s.parity.discount, kSurface[i].discount, 1e-4) << s.expiry;
    EXPECT_FALSE(s.tx_settlement.has_value());
    EXPECT_EQ(s.quotes.forward, s.parity.forward);
    EXPECT_EQ(s.quotes.discount, s.parity.discount);
    EXPECT_EQ(s.quotes.expiry, s.years);
    ASSERT_EQ(s.quotes.quotes.size(), s.rows.rows.size());
    ASSERT_FALSE(s.rows.rows.empty());
    for (std::size_t j = 0; j < s.rows.rows.size(); ++j) {
      const auto& row = s.rows.rows[j];
      const FitQuote& q = s.quotes.quotes[j];
      EXPECT_EQ(q.strike, row.strike);
      EXPECT_EQ(q.price, *row.settlement_price);
      EXPECT_GE(q.price, kMinSettlementPoints);
      EXPECT_EQ(q.type == OptionType::kPut, row.right == taifex::OptionRight::kPut);
      const double tick = settlement_tick(q.price);
      EXPECT_DOUBLE_EQ(q.noise * q.noise,
                       (s.parity.sigma_leg * s.parity.sigma_leg) + (tick * tick / 12.0));
    }
    const ButterflyCounts counts = count_input_butterflies(s.rows);
    EXPECT_EQ(s.input_butterflies.triples, counts.triples);
    EXPECT_EQ(s.input_butterflies.non_convex, counts.non_convex);
  }
}

TEST(BuildChainSlices, TxSettlementIsLookedUp) {
  const auto rows = testing::SyntheticRows(kSurface, 20000.0, 25000.0, 100.0);
  std::vector<taifex::FuturesRow> futures(1);
  futures[0].contract = "TX";
  futures[0].expiry = "202412";
  futures[0].settlement_price = 22831.0;
  const auto slices = BuildChainSlices(rows, futures, kExpiries);
  ASSERT_TRUE(slices.has_value());
  EXPECT_FALSE((*slices)[0].tx_settlement.has_value());
  EXPECT_EQ((*slices)[2].tx_settlement, 22831.0);
}

TEST(BuildChainSlices, ErrorsAreValues) {
  const auto rows = testing::SyntheticRows(kSurface, 20000.0, 25000.0, 100.0);
  const auto wrong_date = testing::SyntheticRows(kSurface, 20000.0, 25000.0, 100.0, 20240103);
  EXPECT_EQ(BuildChainSlices(wrong_date, {}, kExpiries).error().code,
            ChainErrorCode::kWrongTradeDate);
  constexpr std::array<std::string_view, 1> unknown = {"202502"};
  EXPECT_EQ(BuildChainSlices(rows, {}, unknown).error().code, ChainErrorCode::kUnknownExpiry);
  const auto thin = testing::SyntheticRows(kSurface, 22000.0, 22100.0, 100.0);
  const auto parity = BuildChainSlices(thin, {}, kExpiries);
  ASSERT_FALSE(parity.has_value());
  EXPECT_EQ(parity.error().code, ChainErrorCode::kParity);
  EXPECT_EQ(parity.error().expiry, "202411");
  EXPECT_EQ(parity.error().parity_error, ParityError::kTooFewPoints);
  for (const auto code : {ChainErrorCode::kWrongTradeDate, ChainErrorCode::kUnknownExpiry,
                          ChainErrorCode::kParity, ChainErrorCode::kCalibrationSet}) {
    EXPECT_STRNE(ChainErrorName(code), "unknown");
  }
}

TEST(FitChain, FitsEverySliceAndChecksAdjacentPairs) {
  const auto rows = testing::SyntheticRows(kSurface, 20000.0, 25000.0, 100.0);
  const auto slices = BuildChainSlices(rows, {}, kExpiries);
  ASSERT_TRUE(slices.has_value());
  const SurfaceFit surface = FitChain(*slices);
  ASSERT_EQ(surface.slices.size(), 3U);
  for (std::size_t i = 0; i < 3; ++i) {
    const SliceFit& r = surface.slices[i];
    ASSERT_TRUE(r.fit.has_value()) << kExpiries[i];
    EXPECT_EQ(r.fit->quotes, (*slices)[i].quotes.quotes.size());
    EXPECT_EQ(r.svi6_bp, MeanAbsPriceErrorBp((*slices)[i].quotes, r.fit->model_price));
    EXPECT_EQ(r.svi11_vol_points, ImpliedVolRmse(r.fit->model_vol, r.fit->market_vol));
    EXPECT_EQ(r.model_butterflies.triples, (*slices)[i].input_butterflies.triples);
    // A smooth slice free of arbitrage has convex prices.
    EXPECT_EQ(r.model_butterflies.non_convex, 0U) << kExpiries[i];
    EXPECT_TRUE(r.butterfly.violations.empty()) << kExpiries[i];
    // Rounding to 0.1 point is the only noise; the parameters stay close to the truth.
    EXPECT_NEAR(r.fit->params.rho, kSurface[i].params.rho, 0.05) << kExpiries[i];
  }
  ASSERT_EQ(surface.calendar.size(), 2U);
  EXPECT_EQ(surface.calendar[0].earlier, 0U);
  EXPECT_EQ(surface.calendar[0].later, 1U);
  EXPECT_EQ(surface.calendar[1].earlier, 1U);
  EXPECT_EQ(surface.calendar[1].later, 2U);
  EXPECT_TRUE(surface.calendar[0].report.violations.empty());
  EXPECT_TRUE(surface.calendar[1].report.violations.empty());
}

TEST(FitChain, ACrossingIsReportedAndTheShorterExpiryComesFirstInEitherOrder) {
  std::vector<SyntheticExpiry> crossing = {kSurface[0], kSurface[0]};
  crossing[1].expiry = "202411W4";
  crossing[1].params.a -= 5e-5;  // below the 5-day slice at every k
  const auto rows = testing::SyntheticRows(crossing, 20000.0, 25000.0, 100.0);
  constexpr std::array<std::string_view, 2> reversed = {"202411W4", "202411"};
  const auto slices = BuildChainSlices(rows, {}, reversed);
  ASSERT_TRUE(slices.has_value());
  const SurfaceFit surface = FitChain(*slices);
  ASSERT_EQ(surface.calendar.size(), 1U);
  EXPECT_EQ(surface.calendar[0].earlier, 1U);  // 202411
  EXPECT_EQ(surface.calendar[0].later, 0U);
  bool crossed = false;
  for (const Violation& v : surface.calendar[0].report.violations) {
    crossed = crossed || v.kind == ViolationKind::kCalendarCrossing;
  }
  EXPECT_TRUE(crossed);
}

}  // namespace
}  // namespace aadp::svi
