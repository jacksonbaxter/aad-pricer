// Asserts the known counts on the pinned captures, from docs/NOTES.md and the chain evidence. The
// files are never committed. With no file the tests skip, visibly, and say how to get it.
// The counts, forwards, discount factors and settlements asserted here are derived from the
// chain. Data: TAIFEX via data.gov.tw, Open Government Data License (NOTICE).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "data/calibration_set.hpp"
#include "data/forwards.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"

namespace aadp::taifex {
namespace {

namespace fs = std::filesystem;

// Tests run with the repo root as working directory.
fs::path data_dir() {
  const char* override_dir = std::getenv("AADP_DATA_DIR");
  return override_dir != nullptr ? fs::path(override_dir) : fs::path("data");
}

std::optional<std::string> load(std::string_view name) {
  const fs::path path = data_dir() / name;
  if (!fs::exists(path)) {
    return std::nullopt;
  }
  auto bytes = read_binary_file(path);
  EXPECT_TRUE(bytes.has_value()) << bytes.error();
  return bytes ? std::optional<std::string>(std::move(*bytes)) : std::nullopt;
}

// A macro because GTEST_SKIP has to return from the test body itself.
// NOLINTBEGIN(cppcoreguidelines-macro-usage,bugprone-macro-parentheses): declares a named variable.
#define AADP_LOAD_OR_SKIP(variable, name)                                            \
  const std::optional<std::string> variable = load(name);                            \
  if (!variable) {                                                                   \
    GTEST_SKIP() << "SKIPPED: " << (data_dir() / (name)).string()                    \
                 << " is absent, so this real-file test did not run. Fetch it with " \
                    "scripts/fetch_data.sh.";                                        \
  }
// NOLINTEND(cppcoreguidelines-macro-usage,bugprone-macro-parentheses)

constexpr std::string_view kOptionsFile = "DailyMarketReportOpt.csv";
constexpr std::string_view kFuturesFile = "DailyMarketReportFut.csv";

const std::vector<ExpiryForward> kPublishedForwards = {{.expiry = "202411", .forward = 22761.47},
                                                       {.expiry = "202411W4", .forward = 22786.07},
                                                       {.expiry = "202412", .forward = 22792.92}};

std::vector<OptionRow> txo_rows(const OptionsFile& file) {
  std::vector<OptionRow> rows;
  std::ranges::copy_if(file.rows, std::back_inserter(rows),
                       [](const OptionRow& row) { return row.contract == "TXO"; });
  return rows;
}

TEST(RealOptions, ReadsEveryRowWithoutError) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  EXPECT_TRUE(file->row_errors.empty()) << "first at line " << file->row_errors.front().line;
  EXPECT_EQ(file->rows.size(), 7388U);
  EXPECT_TRUE(std::ranges::all_of(file->rows,
                                  [](const OptionRow& row) { return row.trade_date == 20241115; }));
}

TEST(RealOptions, SessionCountsOfTxo) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  const auto rows = txo_rows(*file);
  EXPECT_EQ(rows.size(), 2006U);
  const auto regular = std::ranges::count_if(
      rows, [](const OptionRow& row) { return row.session == Session::kRegular; });
  const auto after_hours = std::ranges::count_if(
      rows, [](const OptionRow& row) { return row.session == Session::kAfterHours; });
  EXPECT_EQ(regular, 1008);
  EXPECT_EQ(after_hours, 998);
}

TEST(RealOptions, SixExpiriesAndTheirRowCounts) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  std::map<std::string, int> regular;
  std::map<std::string, int> after_hours;
  for (const OptionRow& row : txo_rows(*file)) {
    ++(row.session == Session::kRegular ? regular : after_hours)[row.expiry];
  }
  const std::map<std::string, int> expected_regular = {{"202411", 234}, {"202411W4", 134},
                                                       {"202412", 230}, {"202501", 158},
                                                       {"202503", 138}, {"202506", 114}};
  const std::map<std::string, int> expected_after_hours = {{"202411", 230}, {"202411W4", 128},
                                                           {"202412", 230}, {"202501", 158},
                                                           {"202503", 138}, {"202506", 114}};
  EXPECT_EQ(regular, expected_regular);
  EXPECT_EQ(after_hours, expected_after_hours);
}

TEST(RealOptions, SettlementsExistOnlyInTheRegularSession) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  for (const OptionRow& row : txo_rows(*file)) {
    if (row.session == Session::kRegular) {
      EXPECT_TRUE(row.settlement_price.has_value()) << "line " << row.line;
      EXPECT_TRUE(row.open_interest.has_value()) << "line " << row.line;
    } else {
      EXPECT_FALSE(row.settlement_price.has_value()) << "line " << row.line;
      EXPECT_FALSE(row.open_interest.has_value()) << "line " << row.line;
    }
  }
}

TEST(RealOptions, CallsAndPutsPairUpPerStrike) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  const auto calls = std::ranges::count_if(
      file->rows, [](const OptionRow& row) { return row.right == OptionRight::kCall; });
  EXPECT_EQ(calls, 3694);
  EXPECT_EQ(static_cast<std::size_t>(calls) * 2, file->rows.size());
}

TEST(RealOptions, FinalSetsMatchThePublishedCounts) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  const auto slices = select_calibration_set(file->rows, kPublishedForwards);
  ASSERT_TRUE(slices.has_value());
  ASSERT_EQ(slices->size(), 3U);

  struct Expected {
    std::size_t n;
    std::ptrdiff_t puts;
    std::ptrdiff_t calls;
  };
  const std::vector<Expected> expected = {{.n = 65, .puts = 38, .calls = 27},
                                          {.n = 51, .puts = 26, .calls = 25},
                                          {.n = 64, .puts = 38, .calls = 26}};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const CalibrationSlice& slice = (*slices)[i];
    EXPECT_EQ(slice.expiry, kPublishedForwards[i].expiry);
    EXPECT_EQ(slice.rows.size(), expected[i].n) << slice.expiry;
    const auto puts = std::ranges::count_if(
        slice.rows, [](const OptionRow& row) { return row.right == OptionRight::kPut; });
    EXPECT_EQ(puts, expected[i].puts) << slice.expiry;
    EXPECT_EQ(static_cast<std::ptrdiff_t>(slice.rows.size()) - puts, expected[i].calls)
        << slice.expiry;
    for (const OptionRow& row : slice.rows) {
      EXPECT_EQ(row.session, Session::kRegular) << "line " << row.line;
    }
  }
}

TEST(RealOptions, ThresholdDropsTwentyNineRowsFromTheNearExpiryOnly) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  // Counted directly from the rows, on the filter's other conditions, so this is independent of
  // the filter's threshold.
  std::map<std::string, int> below_threshold;
  for (const OptionRow& row : txo_rows(*file)) {
    if (row.session != Session::kRegular || row.volume.value_or(0) == 0 ||
        !row.last_trade_price.has_value() || row.settlement_price != row.last_trade_price ||
        *row.settlement_price >= kMinSettlementPoints) {
      continue;
    }
    for (const ExpiryForward& entry : kPublishedForwards) {
      const bool otm =
          row.right == OptionRight::kPut ? row.strike < entry.forward : row.strike > entry.forward;
      if (row.expiry == entry.expiry && otm) {
        ++below_threshold[entry.expiry];
      }
    }
  }
  const std::map<std::string, int> expected = {{"202411", 29}};
  EXPECT_EQ(below_threshold, expected);
}

TEST(RealFutures, ReadsEveryRowWithoutError) {
  AADP_LOAD_OR_SKIP(bytes, kFuturesFile);
  const auto file = read_futures(*bytes);
  ASSERT_TRUE(file.has_value());
  EXPECT_TRUE(file->row_errors.empty()) << "first at line " << file->row_errors.front().line;
  EXPECT_EQ(file->rows.size(), 2118U);
  EXPECT_TRUE(std::ranges::all_of(
      file->rows, [](const FuturesRow& row) { return row.trade_date == 20241115; }));
}

TEST(RealFutures, SessionsAndSpreads) {
  AADP_LOAD_OR_SKIP(bytes, kFuturesFile);
  const auto file = read_futures(*bytes);
  ASSERT_TRUE(file.has_value());
  const auto regular = std::ranges::count_if(
      file->rows, [](const FuturesRow& row) { return row.session == Session::kRegular; });
  EXPECT_EQ(regular, 1971);
  EXPECT_EQ(file->rows.size() - static_cast<std::size_t>(regular), 147U);
  const auto spreads =
      std::ranges::count_if(file->rows, [](const FuturesRow& row) { return row.calendar_spread; });
  EXPECT_EQ(spreads, 313);
  // Exactly the spread rows carry the spread volume.
  for (const FuturesRow& row : file->rows) {
    EXPECT_EQ(row.spread_order_volume.has_value(), row.calendar_spread) << "line " << row.line;
  }
}

TEST(RealFutures, NullSettlementsAreAfterHoursOutrights) {
  AADP_LOAD_OR_SKIP(bytes, kFuturesFile);
  const auto file = read_futures(*bytes);
  ASSERT_TRUE(file.has_value());
  // 313 "-" on spreads and 124 "NULL", in the raw file.
  const auto missing = std::ranges::count_if(
      file->rows, [](const FuturesRow& row) { return !row.settlement_price.has_value(); });
  EXPECT_EQ(missing, 313 + 124);
  const auto after_hours_outright_missing =
      std::ranges::count_if(file->rows, [](const FuturesRow& row) {
        return !row.calendar_spread && row.session == Session::kAfterHours &&
               !row.settlement_price.has_value();
      });
  EXPECT_EQ(after_hours_outright_missing, 124);
}

TEST(RealFutures, TxSettlementsAndThePublishedForwards) {
  AADP_LOAD_OR_SKIP(bytes, kFuturesFile);
  const auto file = read_futures(*bytes);
  ASSERT_TRUE(file.has_value());
  std::map<std::string, double> settlements;
  for (const FuturesRow& row : file->rows) {
    if (row.contract == "TX" && !row.calendar_spread && row.session == Session::kRegular &&
        row.settlement_price.has_value()) {
      settlements[row.expiry] = *row.settlement_price;
    }
  }
  EXPECT_EQ(settlements["202411"], 22762.0);
  EXPECT_EQ(settlements["202412"], 22792.0);
  EXPECT_EQ(settlements["202501"], 22816.0);
  EXPECT_EQ(settlements["202503"], 22850.0);
  EXPECT_EQ(settlements["202506"], 22890.0);
  EXPECT_FALSE(settlements.contains("202411W4"));

  // docs/NOTES.md: within 0.92 points of TX where a TX month exists. Both numbers there are rounded
  // to two places, so the bound allows for that rounding and no more.
  constexpr double kPublishedGap = 0.92 + 0.005;
  EXPECT_LE(std::abs(22761.47 - settlements["202411"]), kPublishedGap);
  EXPECT_LE(std::abs(22792.92 - settlements["202412"]), kPublishedGap);
}

// Phase 1's chain profile, chain-verify/output/parity.csv in the research cache, at the precision
// it printed (Python repr of a double).
struct PhaseOneParity {
  std::string_view expiry;
  std::size_t n;
  double forward;
  double forward_se;
  double discount;
  double discount_se;
  double rms;
  double max_residual;
};

constexpr std::array<PhaseOneParity, 3> kSettleAll = {{
    {.expiry = "202411",
     .n = 117,
     .forward = 22761.4707319168,
     .forward_se = 0.2385562100758513,
     .discount = 0.999873383900814,
     .discount_se = 9.65029435860093e-05,
     .rms = 2.551570345560012,
     .max_residual = 7.954750803019124},
    {.expiry = "202411W4",
     .n = 67,
     .forward = 22786.069861925156,
     .forward_se = 0.4258963078848856,
     .discount = 0.9994351924587589,
     .discount_se = 0.0003345210195918744,
     .rms = 3.4274123197842625,
     .max_residual = 10.880047132755863},
    {.expiry = "202412",
     .n = 115,
     .forward = 22792.920713091145,
     .forward_se = 0.26451286843265237,
     .discount = 0.998286007115074,
     .discount_se = 7.341975464290616e-05,
     .rms = 2.800740720117704,
     .max_residual = 5.124969426131429},
}};

constexpr std::array<PhaseOneParity, 3> kSettleNear = {{
    {.expiry = "202411",
     .n = 42,
     .forward = 22763.08117467441,
     .forward_se = 0.4257936287831767,
     .discount = 0.9988204620535467,
     .discount_se = 0.0006782183497093036,
     .rms = 2.651966079061519,
     .max_residual = 5.947791137852619},
    {.expiry = "202411W4",
     .n = 40,
     .forward = 22785.977554822875,
     .forward_se = 0.5590381890843759,
     .discount = 0.9997180468471952,
     .discount_se = 0.0009355630585066344,
     .rms = 3.4391602959028202,
     .max_residual = 10.933905558656988},
    {.expiry = "202412",
     .n = 23,
     .forward = 22793.130774171237,
     .forward_se = 0.4784916856975249,
     .discount = 1.0000494071146244,
     .discount_se = 0.0007213503192965455,
     .rms = 2.1927151334131088,
     .max_residual = 4.179841897232109},
}};

// Two implementations of the same centred regression in double differ by rounding, about 1e-13
// here; one strike more or less moves F by far more. docs/NOTES.md, "Stage 6b".
constexpr double kSameRegression = 1e-9;

double round_to(double value, int places) {
  const double scale = std::pow(10.0, places);
  return std::round(value * scale) / scale;
}

void expect_matches(const ParityFit& fit, const PhaseOneParity& want) {
  EXPECT_EQ(fit.points, want.n) << want.expiry;
  EXPECT_NEAR(fit.forward / want.forward, 1.0, kSameRegression) << want.expiry;
  EXPECT_NEAR(fit.forward_se / want.forward_se, 1.0, kSameRegression) << want.expiry;
  EXPECT_NEAR(fit.discount / want.discount, 1.0, kSameRegression) << want.expiry;
  EXPECT_NEAR(fit.discount_se / want.discount_se, 1.0, kSameRegression) << want.expiry;
  EXPECT_NEAR(fit.rms_residual / want.rms, 1.0, kSameRegression) << want.expiry;
  EXPECT_NEAR(fit.max_abs_residual / want.max_residual, 1.0, kSameRegression) << want.expiry;
}

TEST(RealParity, SettleAllReproducesPhaseOne) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  for (const PhaseOneParity& want : kSettleAll) {
    const auto fit = fit_parity(settlement_parity_points(file->rows, want.expiry));
    ASSERT_TRUE(fit.has_value()) << want.expiry;
    expect_matches(*fit, want);
  }
}

TEST(RealParity, PublishedForwardsAndDiscountsToTheirPrintedPlaces) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  // docs/NOTES.md, "Recommended calibration set": F to two places and D to five.
  const std::array<double, 3> forwards = {22761.47, 22786.07, 22792.92};
  const std::array<double, 3> discounts = {0.99987, 0.99944, 0.99829};
  for (std::size_t i = 0; i < kSettleAll.size(); ++i) {
    const auto fit = fit_parity(settlement_parity_points(file->rows, kSettleAll[i].expiry));
    ASSERT_TRUE(fit.has_value());
    EXPECT_EQ(round_to(fit->forward, 2), forwards[i]) << kSettleAll[i].expiry;
    EXPECT_EQ(round_to(fit->discount, 5), discounts[i]) << kSettleAll[i].expiry;
  }
}

// The weights' sigma_leg is settle_all's RMS / sqrt(2), as the method says.
// docs/NOTES.md's 1.87, 2.43 and 1.55 come from settle_near instead: strikes within 5% of the TX
// settlement (of settle_all's F where there is no TX month), with the RMS rounded to two places
// before dividing by sqrt(2). Both are asserted, so the difference is on record.
TEST(RealParity, SigmaLegBySettleAllAndPhaseOnesSettleNearRoute) {
  AADP_LOAD_OR_SKIP(options_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(futures_bytes, kFuturesFile);
  const auto options = read_options(*options_bytes);
  const auto futures = read_futures(*futures_bytes);
  ASSERT_TRUE(options.has_value());
  ASSERT_TRUE(futures.has_value());
  const std::array<double, 3> settle_all_legs = {1.80, 2.42, 1.98};
  const std::array<double, 3> published_legs = {1.87, 2.43, 1.55};
  const std::array<double, 3> unrounded_near_legs = {1.88, 2.43, 1.55};
  for (std::size_t i = 0; i < kSettleAll.size(); ++i) {
    const std::string_view expiry = kSettleAll[i].expiry;
    const auto points = settlement_parity_points(options->rows, expiry);
    const auto all = fit_parity(points);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(round_to(all->sigma_leg, 2), settle_all_legs[i]) << expiry;

    const double reference = tx_settlement(futures->rows, expiry).value_or(all->forward);
    std::vector<ParityPoint> near;
    std::ranges::copy_if(points, std::back_inserter(near), [&](const ParityPoint& p) {
      return std::abs((p.strike / reference) - 1.0) <= 0.05;
    });
    const auto near_fit = fit_parity(near);
    ASSERT_TRUE(near_fit.has_value());
    expect_matches(*near_fit, kSettleNear[i]);
    EXPECT_EQ(round_to(round_to(near_fit->rms_residual, 2) / std::sqrt(2.0), 2), published_legs[i])
        << expiry;
    EXPECT_EQ(round_to(near_fit->sigma_leg, 2), unrounded_near_legs[i]) << expiry;
  }
}

TEST(RealParity, ForwardsWithinPrintedRoundingOfTxWhereATxMonthExists) {
  AADP_LOAD_OR_SKIP(options_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(futures_bytes, kFuturesFile);
  const auto options = read_options(*options_bytes);
  const auto futures = read_futures(*futures_bytes);
  ASSERT_TRUE(options.has_value());
  ASSERT_TRUE(futures.has_value());
  int compared = 0;
  for (const PhaseOneParity& want : kSettleAll) {
    const auto fit = fit_parity(settlement_parity_points(options->rows, want.expiry));
    ASSERT_TRUE(fit.has_value());
    const auto tx = tx_settlement(futures->rows, want.expiry);
    if (!tx) {
      EXPECT_EQ(want.expiry, "202411W4");
      continue;
    }
    ++compared;
    // "Within 0.92 points", to two places: 202412's gap is 0.9207.
    EXPECT_LE(round_to(std::abs(fit->forward - *tx), 2), 0.92) << want.expiry;
  }
  EXPECT_EQ(compared, 2);
}

std::vector<ExpiryForward> regressed_forwards(const OptionsFile& file) {
  std::vector<ExpiryForward> forwards;
  for (const PhaseOneParity& want : kSettleAll) {
    const auto fit = fit_parity(settlement_parity_points(file.rows, want.expiry));
    EXPECT_TRUE(fit.has_value()) << want.expiry;
    forwards.push_back({.expiry = std::string(want.expiry), .forward = fit ? fit->forward : 0.0});
  }
  return forwards;
}

TEST(RealCalibrationSet, RegressedForwardsGiveThePublishedCounts) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  const auto slices = select_calibration_set(file->rows, regressed_forwards(*file));
  ASSERT_TRUE(slices.has_value());
  ASSERT_EQ(slices->size(), 3U);
  const std::array<std::size_t, 3> sizes = {65, 51, 64};
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    EXPECT_EQ((*slices)[i].rows.size(), sizes[i]) << (*slices)[i].expiry;
  }
}

// profile_recommended.txt: fly_triples 61, 47, 60; fly0 15, 19, 17; flyT 3, 13, 7. BENCHMARKS.md
// section 11 has the totals, 168, 51 and 23.
TEST(RealCalibrationSet, InputButterflyCountsMatchPhaseOne) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  const auto slices = select_calibration_set(file->rows, regressed_forwards(*file));
  ASSERT_TRUE(slices.has_value());
  ASSERT_EQ(slices->size(), 3U);
  const std::array<ButterflyCounts, 3> want = {
      {{.triples = 61, .non_convex = 15, .beyond_half_tick = 3},
       {.triples = 47, .non_convex = 19, .beyond_half_tick = 13},
       {.triples = 60, .non_convex = 17, .beyond_half_tick = 7}}};
  ButterflyCounts total;
  for (std::size_t i = 0; i < want.size(); ++i) {
    const ButterflyCounts got = count_input_butterflies((*slices)[i]);
    EXPECT_EQ(got.triples, want[i].triples) << (*slices)[i].expiry;
    EXPECT_EQ(got.non_convex, want[i].non_convex) << (*slices)[i].expiry;
    EXPECT_EQ(got.beyond_half_tick, want[i].beyond_half_tick) << (*slices)[i].expiry;
    total.triples += got.triples;
    total.non_convex += got.non_convex;
    total.beyond_half_tick += got.beyond_half_tick;
  }
  EXPECT_EQ(total.triples, 168U);
  EXPECT_EQ(total.non_convex, 51U);
  EXPECT_EQ(total.beyond_half_tick, 23U);
}

TEST(RealFutures, NegativePricesAppearOnlyOnSpreads) {
  AADP_LOAD_OR_SKIP(bytes, kFuturesFile);
  const auto file = read_futures(*bytes);
  ASSERT_TRUE(file.has_value());
  const auto negative = std::ranges::count_if(
      file->rows, [](const FuturesRow& row) { return row.last_trade_price.value_or(0.0) < 0.0; });
  EXPECT_EQ(negative, 47);
  for (const FuturesRow& row : file->rows) {
    if (!row.calendar_spread) {
      EXPECT_GE(row.last_trade_price.value_or(0.0), 0.0) << "line " << row.line;
    }
  }
}

}  // namespace
}  // namespace aadp::taifex
