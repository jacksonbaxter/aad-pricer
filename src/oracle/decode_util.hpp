// Small parsing helpers for the plan and reference decoders. Nothing here throws, and nothing
// reads past the view it is given.
#ifndef AADP_ORACLE_DECODE_UTIL_HPP_
#define AADP_ORACLE_DECODE_UTIL_HPP_

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aadp::decode {

inline constexpr std::size_t kMaxInputBytes = std::size_t{1} << 24;
inline constexpr std::size_t kMaxLineBytes = 4096;
inline constexpr std::size_t kMaxRows = 100000;

struct DecodeError {
  std::size_t line = 0;  // One-based. Zero for a whole-input error.
  std::string message;
};

// Finite decimal only. Hex, inf and nan are refused here, before strtod could accept them.
inline std::optional<double> ParseDouble(std::string_view text) {
  constexpr std::size_t kMaxNumberBytes = 64;
  if (text.empty() || text.size() >= kMaxNumberBytes) return std::nullopt;
  for (const char c : text) {
    const bool allowed =
        (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-';
    if (!allowed) return std::nullopt;
  }
  char buffer[kMaxNumberBytes];
  text.copy(buffer, text.size());
  buffer[text.size()] = '\0';
  char* end = nullptr;
  const double value = std::strtod(buffer, &end);
  if (end != buffer + text.size() || !std::isfinite(value)) return std::nullopt;
  return value;
}

inline std::optional<std::uint64_t> ParseUnsigned(std::string_view text) {
  std::uint64_t value = 0;
  const char* const last = text.data() + text.size();
  const auto [end, error] = std::from_chars(text.data(), last, value);
  if (text.empty() || error != std::errc{} || end != last) return std::nullopt;
  return value;
}

inline bool IsToken(std::string_view text) {
  if (text.empty()) return false;
  for (const char c : text) {
    const bool allowed = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                         (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-';
    if (!allowed) return false;
  }
  return true;
}

// Printable ASCII without the separator, so a field can never change the column count.
inline bool IsField(std::string_view text) {
  for (const char c : text) {
    if (c < ' ' || c > '~' || c == ',') return false;
  }
  return true;
}

inline std::vector<std::string_view> Split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (;;) {
    const std::size_t end = text.find(separator, start);
    if (end == std::string_view::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, end - start));
    start = end + 1;
  }
}

// Splits on LF. A final newline does not make a final empty line. Lines keep any CR, which then
// fails field checks, so a CRLF file is an error rather than a silent variant.
inline std::vector<std::string_view> Lines(std::string_view input) {
  std::vector<std::string_view> lines = Split(input, '\n');
  if (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

// Shortest that round-trips, because %.17g does.
std::string FormatDouble(double value);

}  // namespace aadp::decode

#endif  // AADP_ORACLE_DECODE_UTIL_HPP_
