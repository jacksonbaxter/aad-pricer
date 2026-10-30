// Decoder for references/oracle_values.csv.
#ifndef AADP_ORACLE_REFERENCES_HPP_
#define AADP_ORACLE_REFERENCES_HPP_

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "oracle/decode_util.hpp"

namespace aadp {

struct Reference {
  std::string id;
  std::string parameters;
  double value = 0.0;
  double b = 0.0;
  std::string method;
  std::string source;

  bool operator==(const Reference&) const = default;
};

struct ReferenceTable {
  std::vector<std::string> comments;
  std::vector<Reference> rows;

  bool operator==(const ReferenceTable&) const = default;
  const Reference* Find(std::string_view id) const;
};

std::expected<ReferenceTable, decode::DecodeError> DecodeReferences(std::string_view input);
std::string EncodeReferences(const ReferenceTable& table);

}  // namespace aadp

#endif  // AADP_ORACLE_REFERENCES_HPP_
