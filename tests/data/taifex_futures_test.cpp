#include "data/taifex_futures.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "taifex_fixture.hpp"

namespace aadp::taifex {
namespace {

using testing::futures_fields;
using testing::join;
using testing::kFuturesHeader;

std::string file_with(const std::string& row) { return kFuturesHeader + "\r\n" + row + "\r\n"; }

ReadError error_of(const testing::FuturesFields& fields) {
  const auto file = read_futures(file_with(join(fields)));
  EXPECT_TRUE(file.has_value());
  EXPECT_TRUE(file->rows.empty());
  EXPECT_EQ(file->row_errors.size(), 1U);
  return file->row_errors.empty() ? ReadError{} : file->row_errors[0];
}

ReadError error_of(std::size_t column, const std::string& value) {
  auto fields = futures_fields();
  fields[column] = value;
  return error_of(fields);
}

testing::FuturesFields spread_fields() {
  auto fields = futures_fields();
  fields[testing::kFutExpiry] = "202401/202402";
  fields[testing::kFutOpen] = "-0.5";
  fields[testing::kFutHigh] = "0.25";
  fields[testing::kFutLow] = "-1.5";
  fields[testing::kFutLast] = "-0.25";
  fields[testing::kFutChange] = "-";
  fields[testing::kFutChangePercent] = "-";
  fields[testing::kFutSettlement] = "-";
  fields[testing::kFutOpenInterest] = "-";
  fields[testing::kFutBid] = "-0.75";
  fields[testing::kFutAsk] = "-0.25";
  fields[testing::kFutHistHigh] = "1.5";
  fields[testing::kFutHistLow] = "-2.5";
  fields[testing::kFutSpreadVolume] = "16";
  return fields;
}

TEST(FuturesReader, ReadsARowField) {
  const auto file = read_futures(file_with(join(futures_fields())));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_TRUE(file->row_errors.empty());
  const FuturesRow& row = file->rows[0];
  EXPECT_EQ(row.line, 2U);
  EXPECT_EQ(row.trade_date, 20240103);
  EXPECT_EQ(row.contract, "ABF");
  EXPECT_EQ(row.expiry, "202401");
  EXPECT_FALSE(row.calendar_spread);
  EXPECT_EQ(row.open_price, 1000.0);
  EXPECT_EQ(row.high_price, 1010.0);
  EXPECT_EQ(row.low_price, 990.0);
  EXPECT_EQ(row.last_trade_price, 1005.0);
  EXPECT_EQ(row.change, 5.0);
  EXPECT_EQ(row.change_percent, 0.5);
  EXPECT_EQ(row.volume, 7);
  EXPECT_EQ(row.settlement_price, 1005.0);
  EXPECT_EQ(row.open_interest, 42);
  EXPECT_EQ(row.last_best_bid, 1004.0);
  EXPECT_EQ(row.last_best_ask, 1006.0);
  EXPECT_EQ(row.historical_high, 1100.0);
  EXPECT_EQ(row.historical_low, 900.0);
  EXPECT_EQ(row.session, Session::kRegular);
  EXPECT_FALSE(row.spread_order_volume);
}

TEST(FuturesReader, TwoCharacterContractIsValid) {
  auto fields = futures_fields();
  fields[testing::kFutContract] = "T2";
  const auto file = read_futures(file_with(join(fields)));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_EQ(file->rows[0].contract, "T2");
}

TEST(FuturesReader, CalendarSpreadMayHaveNegativePrices) {
  const auto file = read_futures(file_with(join(spread_fields())));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  const FuturesRow& row = file->rows[0];
  EXPECT_TRUE(row.calendar_spread);
  EXPECT_EQ(row.expiry, "202401/202402");
  EXPECT_EQ(row.last_trade_price, -0.25);
  EXPECT_EQ(row.last_best_bid, -0.75);
  EXPECT_EQ(row.historical_low, -2.5);
  EXPECT_EQ(row.spread_order_volume, 16);
}

TEST(FuturesReader, WeeklyLegOfASpreadIsValid) {
  auto fields = spread_fields();
  fields[testing::kFutExpiry] = "202401W4/202402";
  const auto file = read_futures(file_with(join(fields)));
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->rows.size(), 1U);
}

TEST(FuturesReader, OutrightMayNotHaveNegativePrices) {
  EXPECT_EQ(error_of(testing::kFutLast, "-1").code, ErrorCode::kNegativeNumber);
  EXPECT_EQ(error_of(testing::kFutBid, "-1").code, ErrorCode::kNegativeNumber);
  EXPECT_EQ(error_of(testing::kFutHistLow, "-1").code, ErrorCode::kNegativeNumber);
}

TEST(FuturesReader, SettlementAndVolumeAreNonNegativeEvenOnASpread) {
  auto fields = spread_fields();
  fields[testing::kFutSettlement] = "-1";
  EXPECT_EQ(error_of(fields).code, ErrorCode::kNegativeNumber);
  fields = spread_fields();
  fields[testing::kFutVolume] = "-1";
  EXPECT_EQ(error_of(fields).code, ErrorCode::kNegativeNumber);
}

TEST(FuturesReader, ChangeIsSigned) {
  auto fields = futures_fields();
  fields[testing::kFutChange] = "-0.65";
  fields[testing::kFutChangePercent] = "-1.25%";
  const auto file = read_futures(file_with(join(fields)));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_EQ(file->rows[0].change, -0.65);
  EXPECT_EQ(file->rows[0].change_percent, -1.25);
}

TEST(FuturesReader, NegativeZeroPercentIsAccepted) {
  auto fields = futures_fields();
  fields[testing::kFutChangePercent] = "-0.00%";
  const auto file = read_futures(file_with(join(fields)));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_EQ(file->rows[0].change_percent, 0.0);
}

TEST(FuturesReader, PercentNeedsItsSign) {
  EXPECT_EQ(error_of(testing::kFutChangePercent, "0.50").code, ErrorCode::kMalformedNumber);
  EXPECT_EQ(error_of(testing::kFutChangePercent, "%").code, ErrorCode::kMalformedNumber);
  EXPECT_EQ(error_of(testing::kFutChangePercent, "0.50%%").code, ErrorCode::kMalformedNumber);
  EXPECT_EQ(error_of(testing::kFutChangePercent, "nan%").code, ErrorCode::kMalformedNumber);
}

TEST(FuturesReader, NullIsMissingOnlyInTheSettlementColumn) {
  auto fields = futures_fields();
  fields[testing::kFutSettlement] = "NULL";
  fields[testing::kFutSession] = testing::kAfterHours;
  const auto file = read_futures(file_with(join(fields)));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_FALSE(file->rows[0].settlement_price);
  EXPECT_EQ(file->rows[0].session, Session::kAfterHours);

  EXPECT_EQ(error_of(testing::kFutLast, "NULL").code, ErrorCode::kMalformedNumber);
  EXPECT_EQ(error_of(testing::kFutOpenInterest, "NULL").code, ErrorCode::kMalformedNumber);
  EXPECT_EQ(error_of(testing::kFutSettlement, "null").code, ErrorCode::kMalformedNumber);
}

TEST(FuturesReader, RejectsNonFiniteSpellingsAndHugeNumbers) {
  for (const std::string text : {"NaN", "nan", "inf", "-inf", "Infinity", "1e400"}) {
    EXPECT_EQ(error_of(testing::kFutSettlement, text).code, ErrorCode::kMalformedNumber) << text;
  }
  EXPECT_EQ(error_of(testing::kFutLast, std::string(400, '9')).code, ErrorCode::kNumberOutOfRange);
}

TEST(FuturesReader, RejectsABadExpiry) {
  EXPECT_EQ(error_of(testing::kFutExpiry, "202401/").code, ErrorCode::kBadExpiry);
  EXPECT_EQ(error_of(testing::kFutExpiry, "/202401").code, ErrorCode::kBadExpiry);
  EXPECT_EQ(error_of(testing::kFutExpiry, "202401/202402/202403").code, ErrorCode::kBadExpiry);
  EXPECT_EQ(error_of(testing::kFutExpiry, "202400").code, ErrorCode::kBadExpiry);
}

TEST(FuturesReader, WrongHeaderFailsTheWholeRead) {
  // A different but well-formed pair, so this is a label mismatch and not bad encoding.
  std::string header = kFuturesHeader;
  header[1] = '\xEA';
  const auto file = read_futures(header + "\r\n");
  ASSERT_FALSE(file.has_value());
  EXPECT_EQ(file.error().code, ErrorCode::kHeaderMismatch);
  EXPECT_EQ(file.error().line, 1U);
}

TEST(FuturesReader, OptionsHeaderIsRejected) {
  EXPECT_FALSE(read_futures(testing::kOptionsHeader + "\r\n").has_value());
}

TEST(FuturesReader, EmptyInputFails) {
  const auto file = read_futures("");
  ASSERT_FALSE(file.has_value());
  EXPECT_EQ(file.error().code, ErrorCode::kEmptyInput);
}

TEST(FuturesReader, BadRowsDoNotStopTheRead) {
  const std::string good = join(futures_fields());
  auto bad = futures_fields();
  bad[testing::kFutVolume] = "x";
  const std::string input =
      kFuturesHeader + "\r\n" + join(bad) + "\r\n" + std::string("1,2\r\n") + good + "\r\n" + good;
  const auto file = read_futures(input);
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->rows.size(), 2U);
  EXPECT_EQ(file->rows[0].line, 4U);
  EXPECT_EQ(file->rows[1].line, 5U);
  ASSERT_EQ(file->row_errors.size(), 2U);
  EXPECT_EQ(file->row_errors[0].line, 2U);
  EXPECT_EQ(file->row_errors[0].column, static_cast<int>(testing::kFutVolume));
  EXPECT_EQ(file->row_errors[1].line, 3U);
  EXPECT_EQ(file->row_errors[1].code, ErrorCode::kWrongColumnCount);
}

TEST(FuturesReader, EmbeddedNulIsARowError) {
  auto fields = futures_fields();
  fields[testing::kFutHalt] = std::string("\0", 1);
  const auto file = read_futures(file_with(join(fields)));
  ASSERT_TRUE(file.has_value());
  EXPECT_TRUE(file->rows.empty());
  ASSERT_EQ(file->row_errors.size(), 1U);
  EXPECT_EQ(file->row_errors[0].code, ErrorCode::kControlCharacter);
}

TEST(FuturesReader, MissingFinalNewlineWithAnEmptyLastFieldStillGivesTheRow) {
  // The last column is empty on outright rows, so the input ends on a comma.
  const auto file = read_futures(kFuturesHeader + "\r\n" + join(futures_fields()));
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->rows.size(), 1U);
  EXPECT_TRUE(file->row_errors.empty());
}

}  // namespace
}  // namespace aadp::taifex
