// Asserts the known counts on the pinned captures, from NOTES.md and the chain evidence. The files
// are never committed. With no file the tests skip, visibly, and say how to get it.

#include <gtest/gtest.h>

#include <algorithm>
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

#define AADP_LOAD_OR_SKIP(variable, name)                                            \
  const std::optional<std::string> variable = load(name);                            \
  if (!variable) {                                                                   \
    GTEST_SKIP() << "SKIPPED: " << (data_dir() / name).string()                      \
                 << " is absent, so this real-file test did not run. Fetch it with " \
                    "scripts/fetch_data.sh.";                                        \
  }

constexpr std::string_view kOptionsFile = "DailyMarketReportOpt.csv";
constexpr std::string_view kFuturesFile = "DailyMarketReportFut.csv";

const std::vector<ExpiryForward> kPublishedForwards = {
    {"202411", 22761.47}, {"202411W4", 22786.07}, {"202412", 22792.92}};

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
  EXPECT_EQ(file->rows.size(), 7388u);
  EXPECT_TRUE(std::ranges::all_of(file->rows,
                                  [](const OptionRow& row) { return row.trade_date == 20241115; }));
}

TEST(RealOptions, SessionCountsOfTxo) {
  AADP_LOAD_OR_SKIP(bytes, kOptionsFile);
  const auto file = read_options(*bytes);
  ASSERT_TRUE(file.has_value());
  const auto rows = txo_rows(*file);
  EXPECT_EQ(rows.size(), 2006u);
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
  ASSERT_EQ(slices->size(), 3u);

  struct Expected {
    std::size_t n;
    std::ptrdiff_t puts;
    std::ptrdiff_t calls;
  };
  const std::vector<Expected> expected = {{65, 38, 27}, {51, 26, 25}, {64, 38, 26}};
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
  EXPECT_EQ(file->rows.size(), 2118u);
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
  EXPECT_EQ(file->rows.size() - static_cast<std::size_t>(regular), 147u);
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

  // NOTES.md: within 0.92 points of TX where a TX month exists. Both numbers there are rounded to
  // two places, so the bound allows for that rounding and no more.
  constexpr double kPublishedGap = 0.92 + 0.005;
  EXPECT_LE(std::abs(22761.47 - settlements["202411"]), kPublishedGap);
  EXPECT_LE(std::abs(22792.92 - settlements["202412"]), kPublishedGap);
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
