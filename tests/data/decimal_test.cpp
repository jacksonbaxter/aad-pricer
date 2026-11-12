#include "data/decimal.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace aadp {
namespace {

TEST(Decimal, ParsesTheGrammar) {
  EXPECT_EQ(parse_decimal("0", SignPolicy::kNonNegative), 0.0);
  EXPECT_EQ(parse_decimal("10", SignPolicy::kNonNegative), 10.0);
  EXPECT_EQ(parse_decimal("0.5", SignPolicy::kNonNegative), 0.5);
  EXPECT_EQ(parse_decimal("1234.5678", SignPolicy::kNonNegative), 1234.5678);
  EXPECT_EQ(parse_decimal("-0.65", SignPolicy::kSigned), -0.65);
  EXPECT_EQ(parse_decimal("999999999999999", SignPolicy::kNonNegative), 999999999999999.0);
  EXPECT_EQ(parse_decimal("0.000000000000001", SignPolicy::kNonNegative), 1e-15);
}

TEST(Decimal, NegativeZeroIsAcceptedWhereSignsAre) {
  // The futures file has "-0.00%".
  const auto value = parse_decimal("-0.00", SignPolicy::kSigned);
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 0.0);
  EXPECT_TRUE(std::signbit(*value));
}

TEST(Decimal, RejectsEverythingOutsideTheGrammar) {
  for (const std::string text :
       {"",    "-",      ".",   ".5",  "5.",    "-.5",  "1.2.3",    "+1",       "--1",
        " 1",  "1 ",     "1e3", "1E3", "1e-3",  "0x10", "0X10",     "NaN",      "nan",
        "NAN", "nan(1)", "inf", "Inf", "INF",   "-inf", "infinity", "Infinity", "007",
        "00",  "01.5",   "-01", "1,5", "1_000", "1%",   "\xA4\x40"}) {
    const auto value = parse_decimal(text, SignPolicy::kSigned);
    ASSERT_FALSE(value.has_value()) << "accepted '" << text << "'";
    EXPECT_EQ(value.error(), NumberError::kMalformed) << text;
  }
}

TEST(Decimal, EmbeddedNulIsMalformed) {
  EXPECT_FALSE(parse_decimal(std::string("1\0", 2), SignPolicy::kSigned).has_value());
  EXPECT_FALSE(parse_decimal(std::string("1\0"
                                         "2",
                                         3),
                             SignPolicy::kSigned)
                   .has_value());
}

TEST(Decimal, RefusesNumbersItCannotConvertExactly) {
  const std::string huge(400, '9');
  const std::vector<std::string> texts = {huge,
                                          "1" + std::string(400, '0'),
                                          "1234567890123456",
                                          "0.1234567890123456",
                                          "0." + std::string(400, '0') + "1",
                                          "12345678.12345678"};
  for (const std::string& text : texts) {
    const auto value = parse_decimal(text, SignPolicy::kSigned);
    ASSERT_FALSE(value.has_value()) << text;
    EXPECT_EQ(value.error(), NumberError::kOutOfRange) << text;
  }
}

TEST(Decimal, SignPolicyIsEnforced) {
  const auto value = parse_decimal("-1", SignPolicy::kNonNegative);
  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error(), NumberError::kNegative);
}

// strtod is correctly rounded on both libcs, and the test runs in the C locale. A mismatch means
// the conversion above is not what its comment claims.
TEST(Decimal, MatchesStrtodExactly) {
  std::mt19937_64 rng(20241115);
  std::uniform_int_distribution<int> digit_count(1, 15);
  std::uniform_int_distribution<int> digit(0, 9);
  for (int trial = 0; trial < 2'000'000; ++trial) {
    const int total = digit_count(rng);
    const int fraction = std::uniform_int_distribution<int>(0, total - 1)(rng);
    const int integer = total - fraction;
    std::string text;
    text += static_cast<char>('1' + digit(rng) % 9);
    for (int i = 1; i < integer; ++i) {
      text += static_cast<char>('0' + digit(rng));
    }
    if (fraction > 0) {
      text += '.';
      for (int i = 0; i < fraction; ++i) {
        text += static_cast<char>('0' + digit(rng));
      }
    }
    const auto value = parse_decimal(text, SignPolicy::kNonNegative);
    ASSERT_TRUE(value.has_value()) << text;
    ASSERT_EQ(*value, std::strtod(text.c_str(), nullptr)) << text;
  }
}

TEST(Decimal, EqualValuesInDifferentSpellingsAreEqualDoubles) {
  EXPECT_EQ(parse_decimal("1", SignPolicy::kNonNegative),
            parse_decimal("1.0", SignPolicy::kNonNegative));
  // 22761.47 is the 202411 parity forward, derived from the chain. Data: TAIFEX via data.gov.tw,
  // Open Government Data License (NOTICE).
  EXPECT_EQ(parse_decimal("22761.47", SignPolicy::kNonNegative),
            parse_decimal("22761.470", SignPolicy::kNonNegative));
}

TEST(Count, ParsesNonNegativeIntegers) {
  EXPECT_EQ(parse_count("0"), 0);
  EXPECT_EQ(parse_count("12947"), 12947);
  EXPECT_EQ(parse_count("999999999999999"), std::int64_t{999999999999999});
}

TEST(Count, RejectsWhatIsNotACount) {
  EXPECT_EQ(parse_count("1.5").error(), NumberError::kMalformed);
  EXPECT_EQ(parse_count("1.0").error(), NumberError::kMalformed);
  EXPECT_EQ(parse_count("-1").error(), NumberError::kNegative);
  EXPECT_EQ(parse_count("1e3").error(), NumberError::kMalformed);
  EXPECT_EQ(parse_count("nan").error(), NumberError::kMalformed);
  EXPECT_EQ(parse_count("9223372036854775808").error(), NumberError::kOutOfRange);
  EXPECT_EQ(parse_count(std::string(400, '9')).error(), NumberError::kOutOfRange);
}

}  // namespace
}  // namespace aadp
