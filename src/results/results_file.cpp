#include "results/results_file.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "results/measure_line.hpp"

namespace aadp::results {
namespace {

constexpr std::string_view kSectionMarker = "@section ";
// Far above any real file, and small enough that a bad input cannot exhaust memory.
constexpr std::size_t kMaxFileBytes = std::size_t{64} * 1024U * 1024U;
// The repeat checks below are quadratic, so these bound the work as well as the file.
constexpr std::size_t kMaxHeaderLines = 1024;
constexpr std::size_t kMaxSections = 1024;

bool valid_key(std::string_view key) {
  return !key.empty() && std::ranges::all_of(key, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

bool valid_section_name(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
  });
}

// Bytes above 0x7f pass, so a CPU model or path in UTF-8 reaches the privacy scan intact.
bool valid_value(std::string_view value) {
  return std::ranges::none_of(
      value, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
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

}  // namespace

std::optional<std::string_view> ResultsFile::value(std::string_view key) const {
  for (const HeaderField& field : header) {
    if (field.key == key) {
      return field.value;
    }
  }
  return std::nullopt;
}

const ResultsSection* ResultsFile::section(std::string_view name) const {
  for (const ResultsSection& entry : sections) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

namespace {

std::optional<std::string> parse_header_line(std::string_view line, ResultsFile& file) {
  const auto colon = line.find(": ");
  if (colon == std::string_view::npos) {
    return "header line without `key: value`";
  }
  const std::string_view key = line.substr(0, colon);
  const std::string_view value = line.substr(colon + 2);
  if (!valid_key(key)) {
    return "bad header key";
  }
  if (!valid_value(value)) {
    return "control character in a header value";
  }
  if (file.value(key)) {
    return "repeated header key " + std::string(key);
  }
  if (file.header.size() == kMaxHeaderLines) {
    return "too many header lines";
  }
  file.header.push_back({.key = std::string(key), .value = std::string(value)});
  return std::nullopt;
}

std::optional<std::string> parse_body_line(std::string_view line, ResultsFile& file) {
  if (line.starts_with(kSectionMarker)) {
    const std::string_view name = line.substr(kSectionMarker.size());
    if (!valid_section_name(name)) {
      return "bad section name";
    }
    if (file.section(name) != nullptr) {
      return "repeated section " + std::string(name);
    }
    if (file.sections.size() == kMaxSections) {
      return "too many sections";
    }
    file.sections.push_back({.name = std::string(name), .body = {}});
    return std::nullopt;
  }
  if (file.sections.empty()) {
    return "data before the first section";
  }
  std::string& body = file.sections.back().body;
  body.append(line);
  body.push_back('\n');
  return std::nullopt;
}

std::optional<std::string> check_writable_header(const ResultsFile& file) {
  for (std::size_t i = 0; i < file.header.size(); ++i) {
    const HeaderField& field = file.header[i];
    if (!valid_key(field.key)) {
      return "bad header key";
    }
    if (!valid_value(field.value)) {
      return "control character in header value of " + field.key;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (file.header[j].key == field.key) {
        return "repeated header key " + field.key;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string> check_writable_body(const ResultsSection& section) {
  if (section.body.contains('\0')) {
    return "NUL in section " + section.name;
  }
  if (!section.body.empty() && section.body.back() != '\n') {
    return "section " + section.name + " does not end in a newline";
  }
  std::string_view rest = section.body;
  std::string_view line;
  while (next_line(rest, line)) {
    if (line.starts_with(kSectionMarker)) {
      return "section " + section.name + " has a line like a section marker";
    }
  }
  return std::nullopt;
}

std::optional<std::string> check_writable_sections(const ResultsFile& file) {
  for (std::size_t i = 0; i < file.sections.size(); ++i) {
    const ResultsSection& section = file.sections[i];
    if (!valid_section_name(section.name)) {
      return "bad section name";
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (file.sections[j].name == section.name) {
        return "repeated section " + section.name;
      }
    }
    if (auto problem = check_writable_body(section)) {
      return problem;
    }
  }
  return std::nullopt;
}

}  // namespace

std::expected<ResultsFile, std::string> parse_results_file(std::string_view text) {
  if (text.size() > kMaxFileBytes) {
    return std::unexpected("file is too large");
  }
  if (text.contains('\0')) {
    return std::unexpected("file has a NUL byte");
  }
  std::string_view line;
  if (!next_line(text, line) || line != kResultsMagic) {
    return std::unexpected("first line is not the results magic");
  }

  ResultsFile file;
  bool header_closed = false;
  while (next_line(text, line)) {
    if (line.empty()) {
      header_closed = true;
      break;
    }
    if (auto problem = parse_header_line(line, file)) {
      return std::unexpected(std::move(*problem));
    }
  }
  if (!header_closed) {
    return std::unexpected("header is not closed by a blank line");
  }

  while (next_line(text, line)) {
    if (auto problem = parse_body_line(line, file)) {
      return std::unexpected(std::move(*problem));
    }
  }
  return file;
}

std::expected<std::string, std::string> write_results_file(const ResultsFile& file) {
  if (file.header.size() > kMaxHeaderLines || file.sections.size() > kMaxSections) {
    return std::unexpected("too many header lines or sections");
  }
  if (auto problem = check_writable_header(file)) {
    return std::unexpected(std::move(*problem));
  }
  if (auto problem = check_writable_sections(file)) {
    return std::unexpected(std::move(*problem));
  }
  std::string out(kResultsMagic);
  out += '\n';
  for (const HeaderField& field : file.header) {
    out += field.key + ": " + field.value + "\n";
  }
  out += '\n';
  for (const ResultsSection& section : file.sections) {
    out += std::string(kSectionMarker) + section.name + "\n" + section.body;
  }
  return out;
}

std::vector<std::string_view> missing_keys(const ResultsFile& file,
                                           std::span<const std::string_view> keys) {
  std::vector<std::string_view> missing;
  for (const std::string_view key : keys) {
    const auto value = file.value(key);
    if (!value || value->empty()) {
      missing.push_back(key);
    }
  }
  return missing;
}

std::vector<std::string_view> missing_stamped_keys(const ResultsFile& file) {
  std::vector<std::string_view> missing = missing_keys(file, kRunHeaderKeys);
  for (const auto keys : {std::span<const std::string_view>(kPerfHeaderKeys),
                          std::span<const std::string_view>(kStampHeaderKeys)}) {
    const auto more = missing_keys(file, keys);
    missing.insert(missing.end(), more.begin(), more.end());
  }
  return missing;
}

bool is_commit_hash(std::string_view text) {
  return text.size() == 40 && std::ranges::all_of(text, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::expected<void, std::string> validate_stamped(const ResultsFile& file) {
  const auto missing = missing_stamped_keys(file);
  if (!missing.empty()) {
    return std::unexpected("header is missing " + std::string(missing.front()));
  }
  const std::string_view claim = file.required("claim");
  if (claim != "none" && claim != "result") {
    return std::unexpected("claim must be none or result");
  }
  const std::string_view label = file.required("host_label");
  if (label != "mac-m4max" && label != "linux-zen1" && label != "other") {
    return std::unexpected("unknown host label");
  }
  const std::string_view reportable = file.required("reportable");
  if (reportable != "yes" && reportable != "no") {
    return std::unexpected("reportable must be yes or no");
  }
  if (file.section(kBenchmarkSection) == nullptr) {
    return std::unexpected("no benchmark section");
  }
  if (reportable == "no") {
    return {};
  }

  if (label == "other") {
    return std::unexpected("host other is never reportable");
  }
  const auto start = parse_measure_line(file.required("measure_start"));
  const auto end = parse_measure_line(file.required("measure_end"));
  if (!start || start->kind != MeasureKind::kStart) {
    return std::unexpected("reportable without a measure.sh start line");
  }
  if (!end || end->kind != MeasureKind::kEnd) {
    return std::unexpected("reportable without a measure.sh end line");
  }
  if (end->exit_status != 0) {
    return std::unexpected("reportable with a non-zero exit");
  }
  const std::string_view tree = file.required("tree_state");
  if (tree != "clean" && tree != "exported") {
    return std::unexpected("reportable from a tree that is not clean");
  }
  if (!is_commit_hash(file.required("commit"))) {
    return std::unexpected("reportable without a commit");
  }
  return {};
}

std::vector<HistogramValue> parse_histogram_body(std::string_view body) {
  std::vector<HistogramValue> values;
  std::string_view line;
  while (next_line(body, line)) {
    const auto space = line.find(' ');
    if (space != std::string_view::npos) {
      values.push_back({.key = std::string(line.substr(0, space)),
                        .value = std::string(line.substr(space + 1))});
    }
  }
  return values;
}

}  // namespace aadp::results
