#ifndef AADP_DATA_CP950_HPP_
#define AADP_DATA_CP950_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

// Structural decoder for Big5/cp950 bytes. It finds unit boundaries and rejects bad pairs. It
// carries no mapping table, so it never produces Unicode: the TAIFEX readers compare the few
// labels they need against constants written as cp950 byte strings. A pair that is well formed
// but unassigned in cp950 therefore passes here, and fails later if it reaches a label check.
namespace aadp::cp950 {

enum class ErrorKind : std::uint8_t {
  kInvalidLeadByte,   // 0x80 or 0xFF, neither ASCII nor a lead byte.
  kInvalidTrailByte,  // A lead byte followed by something that cannot be a trail byte.
  kTruncatedPair,     // A lead byte is the last byte of the input.
};

struct DecodeError {
  std::size_t position;  // Offset of the offending lead byte, always inside the input.
  ErrorKind kind;
};

// One decoded character: one ASCII byte, or a lead and a trail byte.
struct Unit {
  std::size_t offset;
  std::uint8_t length;
};

constexpr bool is_ascii(std::uint8_t byte) { return byte < 0x80; }
constexpr bool is_lead_byte(std::uint8_t byte) { return byte >= 0x81 && byte <= 0xFE; }
constexpr bool is_trail_byte(std::uint8_t byte) {
  return (byte >= 0x40 && byte <= 0x7E) || (byte >= 0xA1 && byte <= 0xFE);
}

// Decodes the unit starting at `offset`, which must be below bytes.size(). Tokenisers must step
// by this: a trail byte can equal ASCII such as '@', '\\' or '|'. Comma cannot, but a scan of raw
// bytes for anything else in that range would cut a pair in two.
std::expected<Unit, DecodeError> decode_unit(std::string_view bytes, std::size_t offset);

// Decodes all of `bytes`, stopping at the first error.
std::expected<std::vector<Unit>, DecodeError> decode(std::string_view bytes);

}  // namespace aadp::cp950

#endif  // AADP_DATA_CP950_HPP_
