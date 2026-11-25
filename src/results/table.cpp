#include "results/table.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "results/aad_table.hpp"
#include "results/json_lite.hpp"
#include "results/measure_line.hpp"
#include "results/report.hpp"
#include "results/results_file.hpp"

namespace aadp::results {
namespace {

constexpr std::size_t kShortCommit = 12;

std::string histogram_value(const std::vector<HistogramValue>& values, std::string_view key) {
  for (const HistogramValue& value : values) {
    if (value.key == key) {
      return value.value;
    }
  }
  return "absent";
}

// A table cell must not be able to open a new cell or a new row.
std::string cell(std::string_view text) {
  std::string out(text);
  std::ranges::replace(out, '|', '/');
  return out;
}

// Takes a file into the table, or says why it cannot go in. The first file names the host.
std::expected<void, std::string> admit_file(const ResultsFile& file, std::string& host,
                                            const TableOptions& options) {
  if (auto valid = validate_stamped(file); !valid) {
    return std::unexpected(valid.error());
  }
  const std::string label(file.required("host_label"));
  if (host.empty()) {
    host = label;
  } else if (host != label) {
    return std::unexpected("refusing to put hosts " + host + " and " + label + " in one table");
  }
  if (file.required("reportable") != "yes" && !options.iteration) {
    return std::unexpected("not reportable: " + std::string(file.required("reportable_reason")));
  }
  return {};
}

std::string list_text(const std::vector<std::string>& items, std::string_view none) {
  if (items.empty()) {
    return std::string(none);
  }
  std::string out;
  for (const std::string& item : items) {
    out += (out.empty() ? "" : ", ") + item;
  }
  return out;
}

// One report section as a table: its title, where it comes from, then a column per report column,
// the unit in the header. Every cell is in a character set without `|`, so cell() is a guard only.
std::string report_table(const ResultsFile& file, const ReportTable& table) {
  std::string out = "#### " + cell(table.title) + "\n\n";
  out += "`" + std::string(kReportPrefix) + table.name + "`, commit `" +
         cell(file.required("commit").substr(0, kShortCommit)) + "`, claim `" +
         cell(file.required("claim")) + "`, started " + cell(file.required("run_started_at")) +
         ". Targets: " + list_text(table.targets, "none") +
         ". Timings: " + (table.timing ? "yes" : "no") +
         ". Derived from the option chain: " + (table.uses_chain ? "yes" : "no") + ".\n\n| row";
  std::string rule = "|---";
  for (const ReportColumn& column : table.columns) {
    out +=
        " | " + cell(column.name) + (column.unit == "none" ? "" : " (" + cell(column.unit) + ")");
    rule += "|---";
  }
  out += " |\n" + rule + "|\n";
  for (const ReportRow& row : table.rows) {
    out += "| " + cell(row.key);
    for (const std::string& value : row.cells) {
      out += " | " + cell(value);
    }
    out += " |\n";
  }
  return out;
}

// The attribution as the file carries it, in an indented code block so that it reaches the
// reader byte for byte.
std::string attribution_block(std::string_view text) {
  std::string out =
      "The tables derived from the option chain keep its terms. The attribution their file "
      "carries, as in `NOTICE`:\n\n";
  std::string_view rest = text;
  while (!rest.empty()) {
    const std::size_t end = rest.find('\n');
    const std::string_view line = rest.substr(0, end);
    out += line.empty() ? "\n" : "    " + std::string(line) + "\n";
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
  }
  return out;
}

// One table row per histogram in an admitted file.
std::expected<std::string, std::string> file_rows(const ResultsFile& file,
                                                  const TableOptions& options) {
  const ResultsSection* benchmark = file.section(kBenchmarkSection);
  if (benchmark == nullptr) {
    return std::unexpected("no benchmark section");
  }
  auto json = parse_json(benchmark->body);
  if (!json) {
    return std::unexpected("benchmark JSON: " + json.error());
  }
  const auto start = parse_measure_line(file.required("measure_start"));
  const auto end = parse_measure_line(file.required("measure_end"));
  if ((!start || !end) && !options.iteration) {
    return std::unexpected("a reportable file has a malformed measure line");
  }
  // An iteration run need not have gone through measure.sh, so its load can be absent.
  const std::string load_start = start ? start->load1 : std::string("absent");
  const std::string load_end = end ? end->load1 : std::string("absent");
  // One host may run a benchmark in two builds (linux-zen1's release and release-avx2), so each
  // row names its file's build and instruction set.
  const std::string build =
      "`" + cell(file.required("simd_cmake_target")) + "`, " + cell(file.required("simd_target"));
  std::string rows;
  bool any_histogram = false;
  for (const ResultsSection& section : file.sections) {
    if (!section.name.starts_with(kHistogramPrefix)) {
      continue;
    }
    any_histogram = true;
    const std::string name = section.name.substr(kHistogramPrefix.size());
    const auto values = parse_histogram_body(section.body);
    const auto mean = benchmark_mean_time(*json, name);
    if (!mean) {
      return std::unexpected("no benchmark entry for " + name);
    }
    rows += "| ";
    rows += cell(name) + " | " + build + " | " + cell(file.required("repetitions")) + " | " +
            cell(histogram_value(values, "batches")) + " | " + cell(*mean) + " | " +
            cell(histogram_value(values, "p50")) + " | " + cell(histogram_value(values, "p99")) +
            " | " + cell(histogram_value(values, "max")) + " | " + cell(load_start) + " | " +
            cell(load_end) + " | " + cell(file.required("commit").substr(0, kShortCommit)) + " | " +
            cell(file.required("claim")) + " |\n";
  }
  if (!any_histogram) {
    return std::unexpected("a results file has no histogram");
  }
  return rows;
}

}  // namespace

std::optional<std::string> benchmark_mean_time(const JsonValue& root, std::string_view name) {
  const JsonValue* list = find_member(root, "benchmarks");
  if (list == nullptr) {
    return std::nullopt;
  }
  std::optional<std::string> single;
  const std::string prefix = std::string(name) + "/";
  for (const JsonValue& entry : list->items) {
    const JsonValue* run_name = find_member(entry, "run_name");
    const JsonValue* real_time = find_member(entry, "real_time");
    if (run_name == nullptr || real_time == nullptr ||
        !(run_name->text == name || run_name->text.starts_with(prefix))) {
      continue;
    }
    const JsonValue* aggregate = find_member(entry, "aggregate_name");
    if (aggregate != nullptr && aggregate->text == "mean") {
      return real_time->text;
    }
    if (aggregate == nullptr) {
      single = real_time->text;
    }
  }
  return single;
}

std::expected<std::string, std::string> render_report_tables(std::span<const ResultsFile> files,
                                                             const TableOptions& options) {
  if (files.empty()) {
    return std::unexpected("no results files");
  }
  std::string host;
  std::string out;
  std::vector<std::string> attributions;
  for (const ResultsFile& file : files) {
    if (auto admitted = admit_file(file, host, options); !admitted) {
      return std::unexpected(admitted.error());
    }
    auto tables = read_report(file);
    if (!tables) {
      return std::unexpected(tables.error().detail);
    }
    for (const ReportTable& table : *tables) {
      out += (out.empty() ? "" : "\n") + report_table(file, table);
    }
    const ResultsSection* attribution = file.section(kAttributionSection);
    const bool uses_chain = std::ranges::any_of(*tables, &ReportTable::uses_chain);
    if (uses_chain && attribution != nullptr &&
        std::ranges::find(attributions, attribution->body) == attributions.end()) {
      attributions.push_back(attribution->body);
    }
  }
  for (const std::string& text : attributions) {
    out += "\n" + attribution_block(text);
  }
  return out;
}

std::expected<std::string, std::string> render_host_table(std::span<const ResultsFile> files,
                                                          const TableOptions& options) {
  if (files.empty()) {
    return std::unexpected("no results files");
  }
  std::string host;
  bool any_without_claim = false;
  std::string not_reportable;
  std::string rows;
  std::vector<ResultsFile> benchmarks;
  std::vector<ResultsFile> reports;
  for (const ResultsFile& file : files) {
    if (auto admitted = admit_file(file, host, options); !admitted) {
      return std::unexpected(admitted.error());
    }
    any_without_claim = any_without_claim || file.required("claim") == "none";
    if (file.required("reportable") != "yes") {
      not_reportable += not_reportable.empty() ? "" : "; ";
      not_reportable += file.required("reportable_reason");
    }
    // admit_file has validated the file, so it is one kind or the other.
    if (file_kind(file) == FileKind::kReport) {
      reports.push_back(file);
      continue;
    }
    benchmarks.push_back(file);
    auto file_text = file_rows(file, options);
    if (!file_text) {
      return std::unexpected(file_text.error());
    }
    rows += *file_text;
  }

  std::string out = options.host_heading ? "### " + host + "\n\n" : std::string();
  if (!not_reportable.empty()) {
    out += "**ITERATION ONLY: not results.** Not reportable: " + cell(not_reportable) +
           ". Nothing here may be quoted.\n\n";
  }
  if (any_without_claim) {
    out += "Rows with claim `none` are harness self-tests, not performance results.\n\n";
  }
  if (!benchmarks.empty()) {
    out +=
        "| benchmark | build | repetitions | batches | mean batch (ns) | p50 (ns) | p99 (ns) | "
        "max (ns) | load1 start | load1 end | commit | claim |\n"
        "|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    out += rows;
    auto aad = render_aad_tables(benchmarks);
    if (!aad) {
      return std::unexpected(aad.error());
    }
    out += *aad;
  }
  if (!reports.empty()) {
    auto tables = render_report_tables(reports, options);
    if (!tables) {
      return std::unexpected(tables.error());
    }
    out += (benchmarks.empty() ? "" : "\n") + *tables;
  }
  return out;
}

std::expected<std::string, std::string> render_tables(std::span<const ResultsFile> files,
                                                      const TableOptions& options) {
  if (files.empty()) {
    return std::unexpected("no results files");
  }
  std::map<std::string, std::vector<ResultsFile>> by_host;
  for (const ResultsFile& file : files) {
    const auto label = file.value("host_label");
    if (!label) {
      return std::unexpected("a results file has no host label");
    }
    by_host[std::string(*label)].push_back(file);
  }
  std::string out;
  for (const auto& [label, group] : by_host) {
    auto table = render_host_table(group, options);
    if (!table) {
      return std::unexpected(table.error());
    }
    out += out.empty() ? "" : "\n";
    out += *table;
  }
  return out;
}

}  // namespace aadp::results
