#include "oracle/references.hpp"

#include <cstdio>
#include <set>

namespace aadp {
namespace {

constexpr std::string_view kHeader = "id,parameters,value,b,method,source";

std::unexpected<decode::DecodeError> Fail(std::size_t line, std::string message) {
  return std::unexpected(decode::DecodeError{line, std::move(message)});
}

}  // namespace

namespace decode {

std::string FormatDouble(double value) {
  char text[40];
  std::snprintf(text, sizeof(text), "%.17g", value);
  return text;
}

}  // namespace decode

const Reference* ReferenceTable::Find(std::string_view id) const {
  for (const Reference& row : rows) {
    if (row.id == id) return &row;
  }
  return nullptr;
}

std::expected<ReferenceTable, decode::DecodeError> DecodeReferences(std::string_view input) {
  if (input.size() > decode::kMaxInputBytes) return Fail(0, "input too large");
  ReferenceTable table;
  std::set<std::string, std::less<>> seen;
  bool header_seen = false;
  std::size_t number = 0;
  for (const std::string_view line : decode::Lines(input)) {
    ++number;
    if (line.size() > decode::kMaxLineBytes) return Fail(number, "line too long");
    if (!line.empty() && line.front() == '#') {
      if (!decode::IsField(line.substr(1)) && line.find(',') == std::string_view::npos) {
        return Fail(number, "bad comment");
      }
      table.comments.emplace_back(line);
      continue;
    }
    if (!header_seen) {
      if (line != kHeader) return Fail(number, "bad header");
      header_seen = true;
      continue;
    }
    if (table.rows.size() >= decode::kMaxRows) return Fail(number, "too many rows");
    const auto fields = decode::Split(line, ',');
    if (fields.size() != 6) return Fail(number, "expected 6 columns");
    if (!decode::IsToken(fields[0])) return Fail(number, "bad id");
    for (const std::size_t column : {std::size_t{1}, std::size_t{4}, std::size_t{5}}) {
      if (fields[column].empty() || !decode::IsField(fields[column])) {
        return Fail(number, "bad text field");
      }
    }
    const auto value = decode::ParseDouble(fields[2]);
    if (!value) return Fail(number, "bad value");
    const auto b = decode::ParseDouble(fields[3]);
    if (!b || *b < 0.0) return Fail(number, "bad b");
    if (!seen.emplace(fields[0]).second) return Fail(number, "duplicate id");
    table.rows.push_back({std::string(fields[0]), std::string(fields[1]), *value, *b,
                          std::string(fields[4]), std::string(fields[5])});
  }
  if (!header_seen) return Fail(0, "missing header");
  if (table.rows.empty()) return Fail(0, "no rows");
  return table;
}

std::string EncodeReferences(const ReferenceTable& table) {
  std::string out;
  for (const std::string& comment : table.comments) out += comment + "\n";
  out += kHeader;
  out += "\n";
  for (const Reference& row : table.rows) {
    out += row.id + "," + row.parameters + "," + decode::FormatDouble(row.value) + "," +
           decode::FormatDouble(row.b) + "," + row.method + "," + row.source + "\n";
  }
  return out;
}

}  // namespace aadp
