#include "data/cp950.hpp"

namespace aadp::cp950 {

std::expected<Unit, DecodeError> decode_unit(std::string_view bytes, std::size_t offset) {
  const auto lead = static_cast<std::uint8_t>(bytes[offset]);
  if (is_ascii(lead)) {
    return Unit{offset, 1};
  }
  if (!is_lead_byte(lead)) {
    return std::unexpected(DecodeError{offset, ErrorKind::kInvalidLeadByte});
  }
  if (offset + 1 == bytes.size()) {
    return std::unexpected(DecodeError{offset, ErrorKind::kTruncatedPair});
  }
  if (!is_trail_byte(static_cast<std::uint8_t>(bytes[offset + 1]))) {
    return std::unexpected(DecodeError{offset, ErrorKind::kInvalidTrailByte});
  }
  return Unit{offset, 2};
}

std::expected<std::vector<Unit>, DecodeError> decode(std::string_view bytes) {
  std::vector<Unit> units;
  units.reserve(bytes.size());
  for (std::size_t offset = 0; offset < bytes.size();) {
    const auto unit = decode_unit(bytes, offset);
    if (!unit) {
      return std::unexpected(unit.error());
    }
    units.push_back(*unit);
    offset += unit->length;
  }
  return units;
}

}  // namespace aadp::cp950
