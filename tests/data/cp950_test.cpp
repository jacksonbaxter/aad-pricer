#include "data/cp950.hpp"

#include <gtest/gtest.h>

#include <string>

namespace aadp::cp950 {
namespace {

TEST(Cp950, DecodesAsciiAndPairs) {
  // A label, then ASCII.
  const std::string bytes = "\xA4\x40\xAF\xEB,x";
  const auto units = decode(bytes);
  ASSERT_TRUE(units.has_value());
  ASSERT_EQ(units->size(), 4u);
  EXPECT_EQ((*units)[0].offset, 0u);
  EXPECT_EQ((*units)[0].length, 2);
  EXPECT_EQ((*units)[1].offset, 2u);
  EXPECT_EQ((*units)[1].length, 2);
  EXPECT_EQ((*units)[2].offset, 4u);
  EXPECT_EQ((*units)[2].length, 1);
  EXPECT_EQ((*units)[3].offset, 5u);
  EXPECT_EQ((*units)[3].length, 1);
}

TEST(Cp950, EmptyInputHasNoUnits) {
  const auto units = decode("");
  ASSERT_TRUE(units.has_value());
  EXPECT_TRUE(units->empty());
}

// Each of these ASCII values is a legal trail byte, so a raw-byte scan would see the punctuation
// where the decoder sees one character.
TEST(Cp950, TrailBytesEqualToAsciiPunctuationStayInsideThePair) {
  for (const char trail : {'@', '[', '\\', ']', '^', '_', '`', '{', '|', '}', '~', 'A', 'z'}) {
    const std::string bytes = std::string("\xA4") + trail + trail;
    const auto units = decode(bytes);
    ASSERT_TRUE(units.has_value()) << "trail " << trail;
    ASSERT_EQ(units->size(), 2u) << "trail " << trail;
    EXPECT_EQ((*units)[0].length, 2);
    // The same byte standing alone is ASCII.
    EXPECT_EQ((*units)[1].offset, 2u);
    EXPECT_EQ((*units)[1].length, 1);
  }
}

TEST(Cp950, ByteClassesMatchTheirRanges) {
  for (int b = 0; b < 256; ++b) {
    const auto byte = static_cast<std::uint8_t>(b);
    EXPECT_EQ(is_ascii(byte), b <= 0x7F) << b;
    EXPECT_EQ(is_lead_byte(byte), b >= 0x81 && b <= 0xFE) << b;
    EXPECT_EQ(is_trail_byte(byte), (b >= 0x40 && b <= 0x7E) || (b >= 0xA1 && b <= 0xFE)) << b;
  }
}

TEST(Cp950, EveryTrailValueIsClassifiedByTheDecoder) {
  for (int b = 0; b < 256; ++b) {
    const std::string bytes = {'\xA4', static_cast<char>(b)};
    const auto units = decode(bytes);
    if ((b >= 0x40 && b <= 0x7E) || (b >= 0xA1 && b <= 0xFE)) {
      ASSERT_TRUE(units.has_value()) << b;
      EXPECT_EQ(units->size(), 1u) << b;
    } else {
      ASSERT_FALSE(units.has_value()) << b;
      EXPECT_EQ(units.error().kind, ErrorKind::kInvalidTrailByte) << b;
      EXPECT_EQ(units.error().position, 0u) << b;
    }
  }
}

TEST(Cp950, CommaCannotBeATrailByte) {
  const auto units = decode("ab\xA4,");
  ASSERT_FALSE(units.has_value());
  EXPECT_EQ(units.error().kind, ErrorKind::kInvalidTrailByte);
  EXPECT_EQ(units.error().position, 2u);
}

TEST(Cp950, RejectsTruncationWithItsPosition) {
  const auto units = decode("abc\xA4\x40\xBD");
  ASSERT_FALSE(units.has_value());
  EXPECT_EQ(units.error().kind, ErrorKind::kTruncatedPair);
  EXPECT_EQ(units.error().position, 5u);
}

TEST(Cp950, RejectsBytesThatCannotLead) {
  for (const char byte : {'\x80', '\xFF'}) {
    const std::string bytes = std::string("ok") + byte + "x";
    const auto units = decode(bytes);
    ASSERT_FALSE(units.has_value());
    EXPECT_EQ(units.error().kind, ErrorKind::kInvalidLeadByte);
    EXPECT_EQ(units.error().position, 2u);
  }
}

TEST(Cp950, LeadByteRangeEndsAreAccepted) {
  EXPECT_TRUE(decode("\x81\x40").has_value());
  EXPECT_TRUE(decode("\xFE\xFE").has_value());
}

TEST(Cp950, EmbeddedNulIsAnAsciiUnitHere) {
  // The structural layer has no opinion on NUL. RecordReader is what rejects it.
  const std::string bytes("a\0b", 3);
  const auto units = decode(bytes);
  ASSERT_TRUE(units.has_value());
  EXPECT_EQ(units->size(), 3u);
}

}  // namespace
}  // namespace aadp::cp950
