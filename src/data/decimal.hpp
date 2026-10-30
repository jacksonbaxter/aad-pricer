#ifndef AADP_DATA_DECIMAL_HPP_
#define AADP_DATA_DECIMAL_HPP_

#include <cstdint>
#include <expected>
#include <string_view>

// Strict decimal parsing for the TAIFEX files, on a grammar narrower than any library parser:
//
//   number := "-"? ("0" | [1-9][0-9]*) ("." [0-9]+)?
//
// No "+", no exponent, no spaces, no "inf" or "nan", no hex, no leading zeros, no ".5" or "5.".
//
// This does not use std::from_chars for double. In libc++ that call lives in the dylib, as
// __from_chars_floating_point, which only recent system libraries export, so a binary built here
// could fail on an older macOS. It also accepts "inf" and "nan", and strtod depends on the locale.
// The code below uses no library number parsing at all, so libc++ and libstdc++ behave the same.
namespace aadp {

enum class NumberError : std::uint8_t {
  kMalformed,   // Outside the grammar.
  kOutOfRange,  // In the grammar, but too many digits to convert exactly.
  kNegative,    // A sign where the field does not allow one.
};

enum class SignPolicy : std::uint8_t { kNonNegative, kSigned };

// Accepts at most 15 significant digits, counting from the first digit that is not the lone
// leading zero, and rejects longer numbers rather than rounding them. A 15-digit mantissa and a
// power of ten up to 1e15 are both exact in binary64, so their quotient is the correctly rounded
// value, the same one strtod gives. Equal text therefore always gives equal doubles.
std::expected<double, NumberError> parse_decimal(std::string_view text, SignPolicy sign);

// A non-negative integer on the same grammar, without a fractional part.
std::expected<std::int64_t, NumberError> parse_count(std::string_view text);

}  // namespace aadp

#endif  // AADP_DATA_DECIMAL_HPP_
