// Properties, on any input:
// - decoding agrees with a separate walker written from the byte ranges alone
// - units cover the input exactly, contiguously, and each is a legal shape
// - the number of units lies between size / 2 and size
// - an error lies inside the input, its kind fits the bytes there, and the prefix before it decodes
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "data/cp950.hpp"
#include "fuzz_taifex_check.hpp"

namespace {

using aadp::cp950::ErrorKind;

bool in_lead_range(std::uint8_t byte) { return byte >= 0x81 && byte <= 0xFE; }

bool in_trail_range(std::uint8_t byte) {
  return (byte >= 0x40 && byte <= 0x7E) || (byte >= 0xA1 && byte <= 0xFE);
}

struct Walk {
  bool accepted = true;
  std::size_t units = 0;
  std::size_t stopped_at = 0;
};

Walk walk(const std::uint8_t* data, std::size_t size) {
  Walk result;
  std::size_t i = 0;
  while (i < size) {
    if (data[i] <= 0x7F) {
      i += 1;
    } else if (in_lead_range(data[i]) && i + 1 < size && in_trail_range(data[i + 1])) {
      i += 2;
    } else {
      result.accepted = false;
      result.stopped_at = i;
      return result;
    }
    ++result.units;
  }
  return result;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  const auto units = aadp::cp950::decode(bytes);
  const Walk reference = walk(data, size);
  AADP_FUZZ_CHECK(units.has_value() == reference.accepted);

  if (units) {
    AADP_FUZZ_CHECK(units->size() == reference.units);
    AADP_FUZZ_CHECK(units->size() <= size);
    AADP_FUZZ_CHECK(units->size() * 2 >= size);
    std::size_t expected_offset = 0;
    for (const aadp::cp950::Unit& unit : *units) {
      AADP_FUZZ_CHECK(unit.offset == expected_offset);
      AADP_FUZZ_CHECK(unit.length == 1 || unit.length == 2);
      if (unit.length == 1) {
        AADP_FUZZ_CHECK(data[unit.offset] <= 0x7F);
      } else {
        AADP_FUZZ_CHECK(in_lead_range(data[unit.offset]));
        AADP_FUZZ_CHECK(in_trail_range(data[unit.offset + 1]));
        // No separator the readers use can hide inside a pair.
        AADP_FUZZ_CHECK(data[unit.offset + 1] != ',' && data[unit.offset + 1] != '\n' &&
                        data[unit.offset + 1] != '\r');
      }
      expected_offset += unit.length;
    }
    AADP_FUZZ_CHECK(expected_offset == size);
    return 0;
  }

  const aadp::cp950::DecodeError error = units.error();
  AADP_FUZZ_CHECK(error.position < size);
  AADP_FUZZ_CHECK(error.position == reference.stopped_at);
  const std::uint8_t byte = data[error.position];
  switch (error.kind) {
    case ErrorKind::kInvalidLeadByte:
      AADP_FUZZ_CHECK(byte == 0x80 || byte == 0xFF);
      break;
    case ErrorKind::kTruncatedPair:
      AADP_FUZZ_CHECK(in_lead_range(byte));
      AADP_FUZZ_CHECK(error.position + 1 == size);
      break;
    case ErrorKind::kInvalidTrailByte:
      AADP_FUZZ_CHECK(in_lead_range(byte));
      AADP_FUZZ_CHECK(error.position + 1 < size);
      AADP_FUZZ_CHECK(!in_trail_range(data[error.position + 1]));
      break;
  }
  AADP_FUZZ_CHECK(aadp::cp950::decode(bytes.substr(0, error.position)).has_value());
  return 0;
}
