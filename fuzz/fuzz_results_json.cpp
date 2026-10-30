// Properties, beyond not crashing: writing is stable after one round, and a key that was dropped
// cannot be found again in what is written.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "results/json_lite.hpp"

#define FUZZ_ASSERT(condition) \
  do {                         \
    if (!(condition)) {        \
      __builtin_trap();        \
    }                          \
  } while (false)

namespace {

constexpr std::array<std::string_view, 2> kDropped = {"host_name", "executable"};

bool has_dropped_key(const aadp::results::JsonValue& value) {
  for (const auto& member : value.members) {
    if (member.key == kDropped[0] || member.key == kDropped[1] || has_dropped_key(member.value)) {
      return true;
    }
  }
  for (const auto& item : value.items) {
    if (has_dropped_key(item)) {
      return true;
    }
  }
  return false;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  auto value = aadp::results::parse_json(text);
  if (!value) {
    return 0;
  }
  const std::string first = aadp::results::write_json(*value);
  const auto reread = aadp::results::parse_json(first);
  FUZZ_ASSERT(reread.has_value());
  FUZZ_ASSERT(aadp::results::write_json(*reread) == first);

  aadp::results::drop_json_keys(*value, kDropped);
  FUZZ_ASSERT(!has_dropped_key(*value));
  const auto cleaned = aadp::results::parse_json(aadp::results::write_json(*value));
  FUZZ_ASSERT(cleaned.has_value());
  FUZZ_ASSERT(!has_dropped_key(*cleaned));
  return 0;
}
