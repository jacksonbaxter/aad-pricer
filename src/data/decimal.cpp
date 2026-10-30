#include "data/decimal.hpp"

#include <array>
#include <cstddef>

namespace aadp {

namespace {

constexpr std::size_t kMaxDigits = 15;
constexpr std::array<double, kMaxDigits + 1> kPowersOfTen = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

struct Scanned {
  bool negative;
  std::int64_t mantissa;
  std::size_t fraction_digits;
};

std::expected<Scanned, NumberError> scan(std::string_view text) {
  std::size_t i = 0;
  const bool negative = i < text.size() && text[i] == '-';
  if (negative) {
    ++i;
  }

  const std::size_t integer_begin = i;
  while (i < text.size() && is_digit(text[i])) {
    ++i;
  }
  const std::size_t integer_digits = i - integer_begin;
  if (integer_digits == 0 || (integer_digits > 1 && text[integer_begin] == '0')) {
    return std::unexpected(NumberError::kMalformed);
  }

  std::size_t fraction_begin = i;
  std::size_t fraction_digits = 0;
  if (i < text.size() && text[i] == '.') {
    fraction_begin = ++i;
    while (i < text.size() && is_digit(text[i])) {
      ++i;
    }
    fraction_digits = i - fraction_begin;
    if (fraction_digits == 0) {
      return std::unexpected(NumberError::kMalformed);
    }
  }
  if (i != text.size()) {
    return std::unexpected(NumberError::kMalformed);
  }

  // Counted before accumulating, so a thousand-digit field cannot overflow the mantissa.
  const bool lone_zero = integer_digits == 1 && text[integer_begin] == '0';
  const std::size_t significant = (lone_zero ? 0 : integer_digits) + fraction_digits;
  if (significant > kMaxDigits) {
    return std::unexpected(NumberError::kOutOfRange);
  }

  std::int64_t mantissa = 0;
  for (std::size_t k = integer_begin; k < integer_begin + integer_digits; ++k) {
    mantissa = mantissa * 10 + (text[k] - '0');
  }
  for (std::size_t k = fraction_begin; k < fraction_begin + fraction_digits; ++k) {
    mantissa = mantissa * 10 + (text[k] - '0');
  }
  return Scanned{negative, mantissa, fraction_digits};
}

}  // namespace

std::expected<double, NumberError> parse_decimal(std::string_view text, SignPolicy sign) {
  const auto scanned = scan(text);
  if (!scanned) {
    return std::unexpected(scanned.error());
  }
  if (scanned->negative && sign == SignPolicy::kNonNegative) {
    return std::unexpected(NumberError::kNegative);
  }
  const double magnitude =
      static_cast<double>(scanned->mantissa) / kPowersOfTen[scanned->fraction_digits];
  return scanned->negative ? -magnitude : magnitude;
}

std::expected<std::int64_t, NumberError> parse_count(std::string_view text) {
  const auto scanned = scan(text);
  if (!scanned) {
    return std::unexpected(scanned.error());
  }
  if (scanned->fraction_digits != 0) {
    return std::unexpected(NumberError::kMalformed);
  }
  if (scanned->negative) {
    return std::unexpected(NumberError::kNegative);
  }
  return scanned->mantissa;
}

}  // namespace aadp
