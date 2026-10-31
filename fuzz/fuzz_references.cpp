// Reference decoder. Same properties as the plan: no crash, a stated error, and every accepted
// table meets its invariants and survives a round trip.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>

#include "oracle/references.hpp"

namespace {

void Require(bool condition) {
  if (!condition) std::abort();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(std::bit_cast<const char*>(data), size);
  const auto table = aadp::DecodeReferences(input);
  if (!table) {
    Require(!table.error().message.empty());
    return 0;
  }
  Require(!table->rows.empty());
  std::set<std::string> ids;
  for (const aadp::Reference& row : table->rows) {
    Require(ids.insert(row.id).second);
    Require(std::isfinite(row.value));
    Require(std::isfinite(row.b) && row.b >= 0.0);
    Require(table->Find(row.id) != nullptr);
    Require(!row.parameters.contains(','));
  }
  const std::string encoded = aadp::EncodeReferences(*table);
  const auto again = aadp::DecodeReferences(encoded);
  Require(again.has_value());
  Require(*again == *table);
  Require(aadp::EncodeReferences(*again) == encoded);
  return 0;
}
