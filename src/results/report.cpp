#include "results/report.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "results/results_file.hpp"

namespace aadp::results {
namespace {

constexpr int kMaxRealDigits = 17;
constexpr std::size_t kMaxRealText = 32;
constexpr std::size_t kMaxNameBytes = 64;
constexpr std::size_t kMaxTitleBytes = 160;
constexpr std::size_t kMaxUnitBytes = 32;
constexpr std::size_t kMaxTargetBytes = 16;
constexpr std::size_t kMaxTargets = 16;
constexpr std::string_view kSectionMarker = "@section ";

std::unexpected<ReportError> fail(ReportProblem problem, std::string detail) {
  return std::unexpected(ReportError{.problem = problem, .detail = std::move(detail)});
}

bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_in(char c, std::string_view set) { return set.contains(c); }

template <typename Allowed>
bool all_within(std::string_view text, std::size_t limit, Allowed allowed) {
  return !text.empty() && text.size() <= limit && std::ranges::all_of(text, allowed);
}

bool valid_table_name(std::string_view name) {
  return all_within(name, kMaxNameBytes,
                    [](char c) { return is_lower(c) || is_digit(c) || c == '_'; });
}

bool valid_column_name(std::string_view name) {
  return valid_table_name(name) && is_lower(name.front());
}

bool valid_unit(std::string_view unit) {
  return all_within(unit, kMaxUnitBytes, [](char c) {
    return is_lower(c) || is_upper(c) || is_digit(c) || is_in(c, "_%/.^-");
  });
}

bool valid_row_key(std::string_view key) {
  return all_within(key, kMaxNameBytes, [](char c) {
    return is_lower(c) || is_upper(c) || is_digit(c) || is_in(c, "._-");
  });
}

bool valid_text_cell(std::string_view text) {
  return all_within(text, kMaxNameBytes, [](char c) {
    return is_lower(c) || is_upper(c) || is_digit(c) || is_in(c, "._+:%=/-");
  });
}

bool valid_target(std::string_view id) {
  return all_within(id, kMaxTargetBytes,
                    [](char c) { return is_upper(c) || is_digit(c) || c == '-'; });
}

// Words and the punctuation of a plain sentence; nothing a Markdown table or a reader's shell
// could take for markup.
bool valid_title(std::string_view title) {
  return all_within(title, kMaxTitleBytes,
                    [](char c) {
                      return is_lower(c) || is_upper(c) || is_digit(c) ||
                             is_in(c, " .,:;()+/%='_-");
                    }) &&
         title.front() != ' ' && title.back() != ' ';
}

std::string_view kind_name(CellKind kind) {
  switch (kind) {
    case CellKind::kReal:
      return "real";
    case CellKind::kInteger:
      return "integer";
    case CellKind::kText:
      return "text";
  }
  return "unknown";
}

std::optional<CellKind> parse_kind(std::string_view text) {
  for (const CellKind kind : {CellKind::kReal, CellKind::kInteger, CellKind::kText}) {
    if (text == kind_name(kind)) {
      return kind;
    }
  }
  return std::nullopt;
}

bool next_line(std::string_view& text, std::string_view& line) {
  if (text.empty()) {
    return false;
  }
  const auto end = text.find('\n');
  line = text.substr(0, end);
  text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
  return true;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> out;
  while (true) {
    const std::size_t at = text.find(separator);
    out.push_back(text.substr(0, at));
    if (at == std::string_view::npos) {
      return out;
    }
    text.remove_prefix(at + 1);
  }
}

std::optional<bool> yes_no(std::string_view text) {
  if (text == "yes") {
    return true;
  }
  if (text == "no") {
    return false;
  }
  return std::nullopt;
}

bool cell_fits(CellKind kind, std::string_view text) {
  switch (kind) {
    case CellKind::kReal:
      return read_real_cell(text).has_value();
    case CellKind::kInteger:
      return read_integer_cell(text).has_value();
    case CellKind::kText:
      return valid_text_cell(text);
  }
  return false;
}

// The fixed lines before the columns, in their order.
enum class Stage : std::uint8_t { kType, kTitle, kTargets, kTiming, kChain, kColumns, kRows };

struct Parser {
  ReportTable table;
  Stage stage = Stage::kType;
  std::set<std::string, std::less<>> keys;

  std::optional<ReportError> fixed_line(std::string_view word, std::string_view rest);
  std::optional<ReportError> column_line(std::string_view rest);
  std::optional<ReportError> row_line(std::string_view rest);
  std::optional<ReportError> line(std::string_view text);
};

std::optional<ReportError> malformed(std::string detail) {
  return ReportError{.problem = ReportProblem::kMalformed, .detail = std::move(detail)};
}

// `targets none`, or ids separated by commas, each once.
std::optional<ReportError> read_targets(std::string_view rest, std::vector<std::string>& targets) {
  if (rest == "none") {
    return std::nullopt;
  }
  for (const std::string_view id : split(rest, ',')) {
    if (!valid_target(id) || std::ranges::find(targets, id) != targets.end()) {
      return malformed("a bad or repeated target id");
    }
    targets.emplace_back(id);
  }
  if (targets.size() > kMaxTargets) {
    return malformed("too many targets");
  }
  return std::nullopt;
}

std::optional<ReportError> Parser::fixed_line(std::string_view word, std::string_view rest) {
  constexpr std::array<std::string_view, 5> kWords = {"type", "title", "targets", "timing",
                                                      "uses_chain"};
  const auto at = static_cast<std::size_t>(stage);
  if (at >= kWords.size() || word != kWords[at]) {
    return malformed("expected `" + std::string(at < kWords.size() ? kWords[at] : "column") +
                     "`, found another line");
  }
  if (stage == Stage::kType) {
    if (rest != "table") {
      return ReportError{.problem = ReportProblem::kUnknownType,
                         .detail = "unknown section type; the only one is table"};
    }
  } else if (stage == Stage::kTitle) {
    if (!valid_title(rest)) {
      return malformed("a title outside the safe character set or too long");
    }
    table.title = std::string(rest);
  } else if (stage == Stage::kTargets) {
    if (auto problem = read_targets(rest, table.targets)) {
      return problem;
    }
  } else {
    const auto flag = yes_no(rest);
    if (!flag) {
      return malformed(std::string(word) + " must be yes or no");
    }
    (stage == Stage::kTiming ? table.timing : table.uses_chain) = *flag;
  }
  stage = static_cast<Stage>(at + 1);
  return std::nullopt;
}

std::optional<ReportError> Parser::column_line(std::string_view rest) {
  if (stage == Stage::kRows) {
    return malformed("a column after the rows");
  }
  const std::vector<std::string_view> parts = split(rest, ' ');
  const auto bad = [](std::string detail) {
    return ReportError{.problem = ReportProblem::kBadColumn, .detail = std::move(detail)};
  };
  if (parts.size() != 3) {
    return bad("a column is a name, a kind and a unit");
  }
  if (!valid_column_name(parts[0])) {
    return bad("a bad column name");
  }
  if (table.column(parts[0])) {
    return bad("repeated column " + std::string(parts[0]));
  }
  const auto kind = parse_kind(parts[1]);
  if (!kind) {
    return bad("column " + std::string(parts[0]) + ": unknown kind");
  }
  if (!valid_unit(parts[2])) {
    return bad("column " + std::string(parts[0]) + ": a bad unit");
  }
  if (table.columns.size() == kMaxReportColumns) {
    return ReportError{.problem = ReportProblem::kTooLarge, .detail = "too many columns"};
  }
  table.columns.push_back(
      {.name = std::string(parts[0]), .kind = *kind, .unit = std::string(parts[2])});
  stage = Stage::kColumns;
  return std::nullopt;
}

std::optional<ReportError> Parser::row_line(std::string_view rest) {
  if (table.columns.empty()) {
    return malformed("a row before any column");
  }
  const std::vector<std::string_view> parts = split(rest, ' ');
  const auto bad = [](ReportProblem problem, std::string detail) {
    return ReportError{.problem = problem, .detail = std::move(detail)};
  };
  if (!valid_row_key(parts[0])) {
    return bad(ReportProblem::kBadRow, "a bad row key");
  }
  const std::string key(parts[0]);
  if (parts.size() - 1 != table.columns.size()) {
    return bad(ReportProblem::kBadRow, "row " + key + ": " + std::to_string(parts.size() - 1) +
                                           " cells for " + std::to_string(table.columns.size()) +
                                           " columns");
  }
  if (!keys.insert(key).second) {
    return bad(ReportProblem::kBadRow, "repeated row key " + key);
  }
  if (table.rows.size() == kMaxReportRows) {
    return bad(ReportProblem::kTooLarge, "too many rows");
  }
  ReportRow row{.key = key, .cells = {}};
  for (std::size_t i = 0; i < table.columns.size(); ++i) {
    const ReportColumn& column = table.columns[i];
    if (!cell_fits(column.kind, parts[i + 1])) {
      return bad(
          ReportProblem::kBadCell,
          "row " + key + ", column " + column.name + ": not a " +
              std::string(kind_name(column.kind)) + " cell" +
              (column.kind == CellKind::kReal ? " (a finite number in its one spelling)" : ""));
    }
    row.cells.emplace_back(parts[i + 1]);
  }
  table.rows.push_back(std::move(row));
  stage = Stage::kRows;
  return std::nullopt;
}

std::optional<ReportError> Parser::line(std::string_view text) {
  const auto space = text.find(' ');
  if (space == std::string_view::npos || space == 0) {
    return malformed("a line that is not `<word> <value>`");
  }
  const std::string_view word = text.substr(0, space);
  const std::string_view rest = text.substr(space + 1);
  if (stage < Stage::kColumns) {
    return fixed_line(word, rest);
  }
  if (word == "column") {
    return column_line(rest);
  }
  if (word == "row") {
    return row_line(rest);
  }
  return malformed("a line that is neither a column nor a row");
}

// What write_results_file would write for the file, counted without writing it.
std::size_t written_size(const ResultsFile& file) {
  std::size_t size = kResultsMagic.size() + 2;  // the magic's newline and the blank line
  for (const HeaderField& field : file.header) {
    size += field.key.size() + field.value.size() + 3;
  }
  for (const ResultsSection& section : file.sections) {
    size += kSectionMarker.size() + section.name.size() + 1 + section.body.size();
  }
  return size;
}

}  // namespace

std::optional<std::size_t> ReportTable::column(std::string_view wanted) const {
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].name == wanted) {
      return i;
    }
  }
  return std::nullopt;
}

const ReportRow* ReportTable::row(std::string_view key) const {
  for (const ReportRow& entry : rows) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

std::expected<FileKind, ReportError> file_kind(const ResultsFile& file) {
  const bool benchmark = file.section(kBenchmarkSection) != nullptr;
  const bool report = std::ranges::any_of(file.sections, [](const ResultsSection& section) {
    return section.name.starts_with(kReportPrefix);
  });
  if (benchmark && report) {
    return fail(ReportProblem::kKind, "both a benchmark file and a report file");
  }
  if (!benchmark && !report) {
    return fail(ReportProblem::kKind, "neither a benchmark file nor a report file");
  }
  return benchmark ? FileKind::kBenchmark : FileKind::kReport;
}

std::string real_cell(double value) {
  if (!std::isfinite(value)) {
    return {};
  }
  std::array<char, 40> text{};
  for (int digits = 1; digits <= kMaxRealDigits; ++digits) {
    std::snprintf(text.data(), text.size(), "%.*g", digits, value);
    if (std::bit_cast<std::uint64_t>(std::strtod(text.data(), nullptr)) ==
        std::bit_cast<std::uint64_t>(value)) {
      break;
    }
  }
  return text.data();
}

std::string integer_cell(std::int64_t value) { return std::to_string(value); }

// strtod, not std::from_chars: libc++ keeps the double overload in a dylib that older macOS
// releases lack (src/data/decimal.hpp). The spelling check below refuses what strtod would
// also take: spaces, hex, inf, nan, a plus sign, needless digits.
std::optional<double> read_real_cell(std::string_view text) {
  if (text.empty() || text.size() > kMaxRealText ||
      !std::ranges::all_of(text, [](char c) { return is_digit(c) || is_in(c, "+-.e"); })) {
    return std::nullopt;
  }
  const std::string copy(text);
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size() || !std::isfinite(value) || real_cell(value) != text) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::int64_t> read_integer_cell(std::string_view text) {
  std::int64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc() || end != text.data() + text.size() ||
      integer_cell(value) != text) {
    return std::nullopt;
  }
  return value;
}

std::optional<double> read_number_cell(const ReportColumn& column, std::string_view text) {
  if (column.kind == CellKind::kReal) {
    return read_real_cell(text);
  }
  if (column.kind == CellKind::kInteger) {
    if (const auto value = read_integer_cell(text)) {
      return static_cast<double>(*value);
    }
  }
  return std::nullopt;
}

std::expected<ResultsSection, ReportError> write_report_section(const ReportTable& table) {
  if (!valid_table_name(table.name)) {
    return fail(ReportProblem::kMalformed, "a bad report section name");
  }
  std::string body = "type table\ntitle " + table.title + "\ntargets ";
  if (table.targets.empty()) {
    body += "none";
  }
  for (std::size_t i = 0; i < table.targets.size(); ++i) {
    body += (i == 0 ? "" : ",") + table.targets[i];
  }
  body += std::string("\ntiming ") + (table.timing ? "yes" : "no") + "\nuses_chain " +
          (table.uses_chain ? "yes" : "no") + "\n";
  for (const ReportColumn& column : table.columns) {
    body += "column " + column.name + " " + std::string(kind_name(column.kind)) + " " +
            column.unit + "\n";
  }
  for (const ReportRow& row : table.rows) {
    body += "row " + row.key;
    for (const std::string& cell : row.cells) {
      body += " " + cell;
    }
    body += '\n';
  }
  ResultsSection section{.name = std::string(kReportPrefix) + table.name, .body = std::move(body)};
  // The reader is the one rule: what it does not read back as this table is not written.
  auto back = parse_report_section(section);
  if (!back) {
    return std::unexpected(back.error());
  }
  if (*back != table) {
    return fail(ReportProblem::kMalformed, "the table does not read back as written");
  }
  return section;
}

std::expected<ReportTable, ReportError> parse_report_section(const ResultsSection& section) {
  std::string_view name = section.name;
  if (!name.starts_with(kReportPrefix)) {
    return fail(ReportProblem::kUnknownSection, "not a report section, nor the attribution");
  }
  name.remove_prefix(kReportPrefix.size());
  if (!valid_table_name(name)) {
    return fail(ReportProblem::kMalformed, "a bad report section name");
  }
  Parser parser;
  parser.table.name = std::string(name);
  std::string_view text = section.body;
  std::string_view line;
  while (next_line(text, line)) {
    if (auto problem = parser.line(line)) {
      return std::unexpected(std::move(*problem));
    }
  }
  if (parser.stage != Stage::kRows) {
    return fail(ReportProblem::kMalformed,
                parser.stage == Stage::kColumns ? "a table with no rows" : "a table cut short");
  }
  return std::move(parser.table);
}

std::expected<std::vector<ReportTable>, ReportError> read_report(const ResultsFile& file) {
  const auto kind = file_kind(file);
  if (!kind) {
    return std::unexpected(kind.error());
  }
  if (*kind != FileKind::kReport) {
    return fail(ReportProblem::kKind, "not a report file");
  }
  if (written_size(file) > kMaxReportFileBytes) {
    return fail(ReportProblem::kTooLarge,
                "a report file larger than " + std::to_string(kMaxReportFileBytes) + " bytes");
  }
  std::vector<ReportTable> tables;
  const ResultsSection* attribution = nullptr;
  bool uses_chain = false;
  for (const ResultsSection& section : file.sections) {
    if (section.name == kAttributionSection) {
      attribution = &section;
      continue;
    }
    if (tables.size() == kMaxReportSections) {
      return fail(ReportProblem::kTooLarge, "too many report sections");
    }
    auto table = parse_report_section(section);
    if (!table) {
      return fail(table.error().problem, "section " + section.name + ": " + table.error().detail);
    }
    uses_chain = uses_chain || table->uses_chain;
    tables.push_back(std::move(*table));
  }
  if (uses_chain) {
    if (attribution == nullptr) {
      return fail(ReportProblem::kNoAttribution,
                  "a section is derived from the chain and the file has no attribution");
    }
    for (const std::string_view fragment : kAttributionFragments) {
      if (!attribution->body.contains(fragment)) {
        return fail(ReportProblem::kNoAttribution, "the attribution lacks a required line");
      }
    }
  }
  return tables;
}

}  // namespace aadp::results
