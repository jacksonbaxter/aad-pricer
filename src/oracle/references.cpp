#include "oracle/references.hpp"

#include <array>
#include <cstdio>
#include <expected>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "oracle/decode_util.hpp"

namespace aadp {
namespace {

constexpr std::string_view kHeader = "id,parameters,value,b,method,source";

std::unexpected<decode::DecodeError> Fail(std::size_t line, std::string message) {
  return std::unexpected(decode::DecodeError{.line = line, .message = std::move(message)});
}

}  // namespace

namespace decode {

std::string FormatDouble(double value) {
  std::array<char, 40> text{};
  std::snprintf(text.data(), text.size(), "%.17g", value);
  return text.data();
}

}  // namespace decode

const Reference* ReferenceTable::Find(std::string_view id) const {
  for (const Reference& row : rows) {
    if (row.id == id) return &row;
  }
  return nullptr;
}

namespace {

std::expected<Reference, std::string> DecodeRow(std::string_view line) {
  const auto fields = decode::Split(line, ',');
  if (fields.size() != 6) return std::unexpected("expected 6 columns");
  if (!decode::IsToken(fields[0])) return std::unexpected("bad id");
  for (const std::size_t column : {std::size_t{1}, std::size_t{4}, std::size_t{5}}) {
    if (fields[column].empty() || !decode::IsField(fields[column])) {
      return std::unexpected("bad text field");
    }
  }
  const auto value = decode::ParseDouble(fields[2]);
  if (!value) return std::unexpected("bad value");
  const auto b = decode::ParseDouble(fields[3]);
  if (!b || *b < 0.0) return std::unexpected("bad b");
  return Reference{.id = std::string(fields[0]),
                   .parameters = std::string(fields[1]),
                   .value = *value,
                   .b = *b,
                   .method = std::string(fields[4]),
                   .source = std::string(fields[5])};
}

}  // namespace

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
      if (!decode::IsPrintable(line)) return Fail(number, "bad comment");
      table.comments.emplace_back(line);
      continue;
    }
    if (!header_seen) {
      if (line != kHeader) return Fail(number, "bad header");
      header_seen = true;
      continue;
    }
    if (table.rows.size() >= decode::kMaxRows) return Fail(number, "too many rows");
    auto row = DecodeRow(line);
    if (!row) return Fail(number, row.error());
    if (!seen.emplace(row->id).second) return Fail(number, "duplicate id");
    table.rows.push_back(*std::move(row));
  }
  if (!header_seen) return Fail(0, "missing header");
  if (table.rows.empty()) return Fail(0, "no rows");
  return table;
}

std::string EncodeReferences(const ReferenceTable& table) {
  std::string out;
  for (const std::string& comment : table.comments) out += comment + "\n";
  out += kHeader;
  out += '\n';
  for (const Reference& row : table.rows) {
    out += row.id + "," + row.parameters + "," + decode::FormatDouble(row.value) + "," +
           decode::FormatDouble(row.b) + "," + row.method + "," + row.source + "\n";
  }
  return out;
}

}  // namespace aadp
