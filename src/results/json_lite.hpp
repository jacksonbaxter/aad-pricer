// Just enough JSON to take fields out of Google Benchmark's output and to read a few values
// back. Scalars keep their source text, so a number is written out exactly as it was read.
#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aadp::results {

struct JsonMember;

struct JsonValue {
  enum class Kind { kObject, kArray, kString, kScalar };
  Kind kind = Kind::kScalar;
  std::string text;  // Scalars: the token. Strings: the contents with escapes left in place
  std::vector<JsonMember> members;
  std::vector<JsonValue> items;
};

struct JsonMember {
  std::string key;
  JsonValue value;
};

// Accepts Google Benchmark's bare NaN and Infinity. Refuses escaped keys, so that a dropped key
// cannot come back spelled differently. Nesting is limited, so this cannot run out of stack.
std::expected<JsonValue, std::string> parse_json(std::string_view text);

// Removes members with these keys, at any depth.
void drop_json_keys(JsonValue& value, std::span<const std::string_view> keys);

std::string write_json(const JsonValue& value);

const JsonValue* find_member(const JsonValue& object, std::string_view key);

}  // namespace aadp::results
