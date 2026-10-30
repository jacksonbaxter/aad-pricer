#include <gtest/gtest.h>

#include <array>
#include <string>

#include "results/json_lite.hpp"

namespace aadp::results {
namespace {

TEST(Json, RoundTripsScalarsExactly) {
  const auto value = parse_json(R"({"a": 1.2340000000000000e+00, "b": [NaN, -Infinity, null]})");
  ASSERT_TRUE(value.has_value()) << value.error();
  const std::string text = write_json(*value);
  EXPECT_NE(text.find("1.2340000000000000e+00"), std::string::npos);
  EXPECT_NE(text.find("NaN"), std::string::npos);
  const auto again = parse_json(text);
  ASSERT_TRUE(again.has_value()) << again.error();
  EXPECT_EQ(write_json(*again), text);
}

TEST(Json, DropsKeysAtAnyDepth) {
  auto value =
      parse_json(R"({"host_name": "x", "a": {"host_name": "y", "b": [{"host_name": 1}]}})");
  ASSERT_TRUE(value.has_value());
  const std::array<std::string_view, 1> keys = {"host_name"};
  drop_json_keys(*value, keys);
  EXPECT_EQ(write_json(*value).find("host_name"), std::string::npos);
}

TEST(Json, RefusesAKeySpelledWithAnEscape) {
  EXPECT_FALSE(parse_json(R"({"host\u005fname": "x"})").has_value());
}

TEST(Json, RefusesMalformedDocuments) {
  for (const std::string_view bad :
       {"", "{", "}", "[", "[1,", "[1 2]", "{\"a\"}", "{\"a\":}", "{a:1}", "\"abc", "\"\\x\"",
        "\"\\u12\"", "01", "1.", "1e", "-", "tru", "{} x", "\"\t\""}) {
    EXPECT_FALSE(parse_json(bad).has_value()) << bad;
  }
}

TEST(Json, RefusesExcessiveNestingInsteadOfOverflowing) {
  const std::string deep(1'000'000, '[');
  EXPECT_FALSE(parse_json(deep).has_value());
}

}  // namespace
}  // namespace aadp::results
