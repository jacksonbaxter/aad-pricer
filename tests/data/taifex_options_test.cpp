#include "data/taifex_options.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <string>

#include "data/taifex_csv.hpp"
#include "taifex_fixture.hpp"

namespace aadp::taifex {
namespace {

using testing::join;
using testing::kOptionsHeader;
using testing::option_fields;

std::string file_with(const std::string& row) { return kOptionsHeader + "\r\n" + row + "\r\n"; }

// Reads one spoiled row and returns its single error.
ReadError error_of(std::size_t column, const std::string& value) {
  auto fields = option_fields();
  fields[column] = value;
  const auto file = read_options(file_with(join(fields)));
  EXPECT_TRUE(file.has_value());
  EXPECT_TRUE(file->rows.empty()) << "accepted '" << value << "' in column " << column;
  EXPECT_EQ(file->row_errors.size(), 1U);
  return file->row_errors.empty() ? ReadError{} : file->row_errors[0];
}

TEST(OptionsReader, ReadsARowField) {
  const auto file = read_options(file_with(join(option_fields())));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_TRUE(file->row_errors.empty());
  const OptionRow& row = file->rows[0];
  EXPECT_EQ(row.line, 2U);
  EXPECT_EQ(row.trade_date, 20240103);
  EXPECT_EQ(row.contract, "ABO");
  EXPECT_EQ(row.expiry, "202401");
  EXPECT_EQ(row.strike, 1000.0);
  EXPECT_EQ(row.right, OptionRight::kCall);
  EXPECT_EQ(row.open_price, 10.5);
  EXPECT_EQ(row.high_price, 11.0);
  EXPECT_EQ(row.low_price, 10.0);
  EXPECT_EQ(row.last_trade_price, 10.5);
  EXPECT_EQ(row.volume, 7);
  EXPECT_EQ(row.settlement_price, 10.5);
  EXPECT_EQ(row.open_interest, 42);
  EXPECT_EQ(row.last_best_bid, 10.0);
  EXPECT_EQ(row.last_best_ask, 11.0);
  EXPECT_EQ(row.historical_high, 12.0);
  EXPECT_EQ(row.historical_low, 9.0);
  EXPECT_FALSE(row.news_halt_flag);
  EXPECT_EQ(row.session, Session::kRegular);
}

TEST(OptionsReader, PreservesSessionAndRight) {
  auto after_hours = option_fields();
  after_hours[testing::kOptSession] = testing::kAfterHours;
  after_hours[testing::kOptRight] = testing::kPut;
  const auto file = read_options(file_with(join(after_hours)));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_EQ(file->rows[0].session, Session::kAfterHours);
  EXPECT_EQ(file->rows[0].right, OptionRight::kPut);
}

TEST(OptionsReader, WeeklyExpiryIsKept) {
  auto fields = option_fields();
  fields[testing::kOptExpiry] = "202401W2";
  const auto file = read_options(file_with(join(fields)));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_EQ(file->rows[0].expiry, "202401W2");
}

TEST(OptionsReader, DashAndEmptyAreMissing) {
  for (const std::string missing : {"-", ""}) {
    auto fields = option_fields();
    for (const auto column :
         {testing::kOptOpen, testing::kOptHigh, testing::kOptLow, testing::kOptLast,
          testing::kOptVolume, testing::kOptSettlement, testing::kOptOpenInterest, testing::kOptBid,
          testing::kOptAsk, testing::kOptHistHigh, testing::kOptHistLow}) {
      fields[column] = missing;
    }
    const auto file = read_options(file_with(join(fields)));
    ASSERT_TRUE(file.has_value());
    ASSERT_EQ(file->rows.size(), 1U) << "missing spelled '" << missing << "'";
    const OptionRow& row = file->rows[0];
    EXPECT_FALSE(row.open_price);
    EXPECT_FALSE(row.high_price);
    EXPECT_FALSE(row.low_price);
    EXPECT_FALSE(row.last_trade_price);
    EXPECT_FALSE(row.volume);
    EXPECT_FALSE(row.settlement_price);
    EXPECT_FALSE(row.open_interest);
    EXPECT_FALSE(row.last_best_bid);
    EXPECT_FALSE(row.last_best_ask);
    EXPECT_FALSE(row.historical_high);
    EXPECT_FALSE(row.historical_low);
  }
}

TEST(OptionsReader, NullIsNotMissingHere) {
  // "NULL" exists only in the futures settlement column.
  EXPECT_EQ(error_of(testing::kOptSettlement, "NULL").code, ErrorCode::kMalformedNumber);
  EXPECT_EQ(error_of(testing::kOptLast, "--").code, ErrorCode::kMalformedNumber);
}

TEST(OptionsReader, RejectsNonFiniteSpellingsAndHugeNumbers) {
  for (const std::string text : {"NaN", "nan", "inf", "INF", "-inf", "Infinity", "1e400", "1e3"}) {
    EXPECT_EQ(error_of(testing::kOptSettlement, text).code, ErrorCode::kMalformedNumber) << text;
  }
  const auto error = error_of(testing::kOptLast, std::string(400, '9'));
  EXPECT_EQ(error.code, ErrorCode::kNumberOutOfRange);
  EXPECT_EQ(error.column, static_cast<int>(testing::kOptLast));
  EXPECT_EQ(error_of(testing::kOptVolume, "1" + std::string(30, '0')).code,
            ErrorCode::kNumberOutOfRange);
}

TEST(OptionsReader, RejectsNegativePricesAndVolumes) {
  EXPECT_EQ(error_of(testing::kOptSettlement, "-1").code, ErrorCode::kNegativeNumber);
  EXPECT_EQ(error_of(testing::kOptVolume, "-1").code, ErrorCode::kNegativeNumber);
  EXPECT_EQ(error_of(testing::kOptOpenInterest, "-1").code, ErrorCode::kNegativeNumber);
}

TEST(OptionsReader, RejectsBadRequiredFields) {
  EXPECT_EQ(error_of(testing::kOptDate, "2024010").code, ErrorCode::kBadDate);
  EXPECT_EQ(error_of(testing::kOptDate, "20240230").code, ErrorCode::kBadDate);
  EXPECT_EQ(error_of(testing::kOptDate, "20231301").code, ErrorCode::kBadDate);
  EXPECT_EQ(error_of(testing::kOptDate, "-").code, ErrorCode::kBadDate);
  EXPECT_EQ(error_of(testing::kOptContract, "").code, ErrorCode::kBadContract);
  EXPECT_EQ(error_of(testing::kOptContract, "abo").code, ErrorCode::kBadContract);
  EXPECT_EQ(error_of(testing::kOptContract, "ABCDEFGHI").code, ErrorCode::kBadContract);
  EXPECT_EQ(error_of(testing::kOptContract, "\xA4\x40").code, ErrorCode::kBadContract);
  EXPECT_EQ(error_of(testing::kOptExpiry, "2024").code, ErrorCode::kBadExpiry);
  EXPECT_EQ(error_of(testing::kOptExpiry, "202413").code, ErrorCode::kBadExpiry);
  EXPECT_EQ(error_of(testing::kOptExpiry, "202401W9").code, ErrorCode::kBadExpiry);
  EXPECT_EQ(error_of(testing::kOptExpiry, "202401/202402").code, ErrorCode::kBadExpiry);
  EXPECT_EQ(error_of(testing::kOptStrike, "-").code, ErrorCode::kMissingField);
  EXPECT_EQ(error_of(testing::kOptStrike, "").code, ErrorCode::kMissingField);
  EXPECT_EQ(error_of(testing::kOptStrike, "0").code, ErrorCode::kMalformedNumber);
  EXPECT_EQ(error_of(testing::kOptRight, "C").code, ErrorCode::kBadRight);
  EXPECT_EQ(error_of(testing::kOptRight, std::string(testing::kRegular)).code,
            ErrorCode::kBadRight);
  EXPECT_EQ(error_of(testing::kOptSession, "").code, ErrorCode::kBadSession);
  EXPECT_EQ(error_of(testing::kOptSession, std::string(testing::kCall)).code,
            ErrorCode::kBadSession);
}

TEST(OptionsReader, EmptyInputFails) {
  const auto file = read_options("");
  ASSERT_FALSE(file.has_value());
  EXPECT_EQ(file.error().code, ErrorCode::kEmptyInput);
}

TEST(OptionsReader, WrongHeaderFailsTheWholeRead) {
  // A different but well-formed pair, so this is a label mismatch and not bad encoding.
  std::string header = kOptionsHeader;
  header[1] = '\xEA';
  const auto file = read_options(header + "\r\n" + join(option_fields()) + "\r\n");
  ASSERT_FALSE(file.has_value());
  EXPECT_EQ(file.error().code, ErrorCode::kHeaderMismatch);
  EXPECT_EQ(file.error().line, 1U);
  EXPECT_EQ(file.error().column, 0);
}

TEST(OptionsReader, HeaderWithAnotherColumnLabelNamesTheColumn) {
  // Swaps the bytes of the volume label for the settlement label.
  const std::string volume = "\xA6\xA8\xA5\xE6\xB6\x71";
  std::string header = kOptionsHeader;
  header.replace(header.find(volume), volume.size(), "\xB5\xB2\xBA\xE2\xBB\xF9");
  const auto file = read_options(header + "\r\n");
  ASSERT_FALSE(file.has_value());
  EXPECT_EQ(file.error().code, ErrorCode::kHeaderMismatch);
  EXPECT_EQ(file.error().column, static_cast<int>(testing::kOptVolume));
}

TEST(OptionsReader, HeaderWithWrongColumnCountFails) {
  const auto fewer = read_options(kOptionsHeader.substr(0, kOptionsHeader.rfind(',')) + "\r\n");
  ASSERT_FALSE(fewer.has_value());
  EXPECT_EQ(fewer.error().code, ErrorCode::kHeaderMismatch);
  const auto more = read_options(kOptionsHeader + ",extra\r\n");
  ASSERT_FALSE(more.has_value());
  EXPECT_EQ(more.error().code, ErrorCode::kHeaderMismatch);
}

TEST(OptionsReader, FuturesHeaderIsRejected) {
  EXPECT_FALSE(read_options(testing::kFuturesHeader + "\r\n").has_value());
}

TEST(OptionsReader, HeaderOnlyFileHasNoRows) {
  for (const std::string& input : {kOptionsHeader, kOptionsHeader + "\r\n"}) {
    const auto file = read_options(input);
    ASSERT_TRUE(file.has_value());
    EXPECT_TRUE(file->rows.empty());
    EXPECT_TRUE(file->row_errors.empty());
  }
}

TEST(OptionsReader, BrokenHeaderEncodingFails) {
  const auto file = read_options(kOptionsHeader.substr(0, 3));
  ASSERT_FALSE(file.has_value());
  EXPECT_EQ(file.error().code, ErrorCode::kInvalidEncoding);
  EXPECT_LT(file.error().position, 3U);
}

TEST(OptionsReader, WrongColumnCountIsARowError) {
  const std::string good = join(option_fields());
  const std::string input = kOptionsHeader + "\r\n" + good + "\r\n" +
                            good.substr(0, good.find(',')) + "\r\n" + good + ",extra\r\n" + good +
                            "\r\n";
  const auto file = read_options(input);
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->rows.size(), 2U);
  EXPECT_EQ(file->rows[0].line, 2U);
  EXPECT_EQ(file->rows[1].line, 5U);
  ASSERT_EQ(file->row_errors.size(), 2U);
  EXPECT_EQ(file->row_errors[0].code, ErrorCode::kWrongColumnCount);
  EXPECT_EQ(file->row_errors[0].line, 3U);
  EXPECT_EQ(file->row_errors[1].code, ErrorCode::kWrongColumnCount);
  EXPECT_EQ(file->row_errors[1].line, 4U);
}

TEST(OptionsReader, BlankLineInTheMiddleIsARowError) {
  const std::string good = join(option_fields());
  const auto file = read_options(kOptionsHeader + "\r\n" + good + "\r\n\r\n" + good + "\r\n");
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->rows.size(), 2U);
  ASSERT_EQ(file->row_errors.size(), 1U);
  EXPECT_EQ(file->row_errors[0].line, 3U);
}

TEST(OptionsReader, RowErrorCarriesLineColumnAndPosition) {
  auto bad = option_fields();
  bad[testing::kOptSettlement] = "oops";
  const std::string prefix = kOptionsHeader + "\r\n" + join(option_fields()) + "\r\n";
  const std::string input = prefix + join(bad) + "\r\n" + join(option_fields()) + "\r\n";
  const auto file = read_options(input);
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->rows.size(), 2U);
  ASSERT_EQ(file->row_errors.size(), 1U);
  const ReadError& error = file->row_errors[0];
  EXPECT_EQ(error.code, ErrorCode::kMalformedNumber);
  EXPECT_EQ(error.line, 3U);
  EXPECT_EQ(error.column, static_cast<int>(testing::kOptSettlement));
  EXPECT_EQ(input.substr(error.position, 4), "oops");
}

TEST(OptionsReader, TrailBytesEqualToAsciiDoNotSplitOrLeak) {
  // A pair whose trail is '|', then a pair whose trail is '\\', inside a free-text field.
  auto fields = option_fields();
  fields[testing::kOptHalt] = "\xA4|\xA4\\";
  const auto file = read_options(file_with(join(fields)));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_TRUE(file->rows[0].news_halt_flag);
}

TEST(OptionsReader, LeadByteBeforeACommaIsAnEncodingError) {
  // A raw split on commas would call this 18 clean fields.
  auto fields = option_fields();
  fields[testing::kOptHalt] = "\xA4";
  const auto file = read_options(file_with(join(fields)) + join(option_fields()) + "\r\n");
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->rows.size(), 1U);
  ASSERT_EQ(file->row_errors.size(), 1U);
  EXPECT_EQ(file->row_errors[0].code, ErrorCode::kInvalidEncoding);
  EXPECT_EQ(file->row_errors[0].line, 2U);
}

TEST(OptionsReader, LeadByteAtLineEndDoesNotEatTheNextLine) {
  const std::string good = join(option_fields());
  const std::string input = kOptionsHeader + "\r\n" + good + "\xA4\n" + good + "\r\n";
  const auto file = read_options(input);
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_EQ(file->rows[0].line, 3U);
  ASSERT_EQ(file->row_errors.size(), 1U);
  EXPECT_EQ(file->row_errors[0].code, ErrorCode::kInvalidEncoding);
  EXPECT_EQ(file->row_errors[0].line, 2U);
}

TEST(OptionsReader, TruncatedPairAtEndOfInputIsARowError) {
  std::string input = file_with(join(option_fields())) + join(option_fields());
  input.resize(input.size() - 1);  // Cuts the session label in half.
  const auto file = read_options(input);
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->rows.size(), 1U);
  ASSERT_EQ(file->row_errors.size(), 1U);
  EXPECT_EQ(file->row_errors[0].code, ErrorCode::kInvalidEncoding);
  EXPECT_EQ(file->row_errors[0].position, input.size() - 1);
}

TEST(OptionsReader, EmbeddedNulIsARowError) {
  auto fields = option_fields();
  fields[testing::kOptHalt] = std::string("a\0b", 3);
  const auto file = read_options(file_with(join(fields)) + join(option_fields()) + "\r\n");
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->rows.size(), 1U);
  ASSERT_EQ(file->row_errors.size(), 1U);
  EXPECT_EQ(file->row_errors[0].code, ErrorCode::kControlCharacter);
}

TEST(OptionsReader, NulInANumberCannotTruncateIt) {
  auto fields = option_fields();
  fields[testing::kOptSettlement] = std::string("10\0.5", 5);
  const auto file = read_options(file_with(join(fields)));
  ASSERT_TRUE(file.has_value());
  EXPECT_TRUE(file->rows.empty());
  ASSERT_EQ(file->row_errors.size(), 1U);
  EXPECT_EQ(file->row_errors[0].code, ErrorCode::kControlCharacter);
}

TEST(OptionsReader, AcceptsCrlfAndLfLineEnds) {
  const std::string good = join(option_fields());
  for (const std::string end : {"\r\n", "\n"}) {
    std::string input(kOptionsHeader);
    for (const std::string& line : {end, good + end, good + end}) {
      input += line;
    }
    const auto file = read_options(input);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->rows.size(), 2U);
    EXPECT_TRUE(file->row_errors.empty());
  }
}

TEST(OptionsReader, BareCrIsARowError) {
  const std::string good = join(option_fields());
  const auto file = read_options(kOptionsHeader + "\r\n" + good + "\r" + good + "\r\n");
  ASSERT_TRUE(file.has_value());
  EXPECT_TRUE(file->rows.empty());
  ASSERT_EQ(file->row_errors.size(), 1U);
  EXPECT_EQ(file->row_errors[0].code, ErrorCode::kControlCharacter);
}

TEST(OptionsReader, TrailingCrAtEndOfInputIsARowError) {
  const auto file = read_options(kOptionsHeader + "\r\n" + join(option_fields()) + "\r");
  ASSERT_TRUE(file.has_value());
  EXPECT_TRUE(file->rows.empty());
  ASSERT_EQ(file->row_errors.size(), 1U);
  EXPECT_EQ(file->row_errors[0].code, ErrorCode::kControlCharacter);
}

TEST(OptionsReader, MissingFinalNewlineStillGivesTheLastRow) {
  const auto file = read_options(kOptionsHeader + "\r\n" + join(option_fields()));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_EQ(file->rows[0].session, Session::kRegular);
}

TEST(OptionsReader, ErrorOnAnEmptyLastFieldStaysInsideTheInput) {
  auto fields = option_fields();
  fields[testing::kOptSession] = "";
  const std::string input = kOptionsHeader + "\r\n" + join(fields);
  const auto file = read_options(input);
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->row_errors.size(), 1U);
  EXPECT_LT(file->row_errors[0].position, input.size());
}

TEST(OptionsReader, NumbersAreAlwaysFinite) {
  const auto file = read_options(file_with(join(option_fields())));
  ASSERT_TRUE(file.has_value());
  ASSERT_EQ(file->rows.size(), 1U);
  EXPECT_TRUE(std::isfinite(file->rows[0].strike));
  const auto settlement = file->rows[0].settlement_price;
  if (!settlement) FAIL() << "no settlement price";
  EXPECT_TRUE(std::isfinite(*settlement));
}

}  // namespace
}  // namespace aadp::taifex
