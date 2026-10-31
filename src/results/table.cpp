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

#include "results/json_lite.hpp"
#include "results/measure_line.hpp"
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

// The mean across repetitions, or the single run's value when there was one repetition.
std::optional<std::string> mean_time(const JsonValue& root, std::string_view name) {
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

// Takes a file into the table, or says why it cannot go in. The first file names the host.
std::expected<void, std::string> admit_file(const ResultsFile& file, std::string& host) {
  if (auto valid = validate_stamped(file); !valid) {
    return std::unexpected(valid.error());
  }
  const std::string label(file.required("host_label"));
  if (host.empty()) {
    host = label;
  } else if (host != label) {
    return std::unexpected("refusing to put hosts " + host + " and " + label + " in one table");
  }
  if (file.required("reportable") != "yes") {
    return std::unexpected("not reportable: " + std::string(file.required("reportable_reason")));
  }
  return {};
}

// One table row per histogram in an admitted file.
std::expected<std::string, std::string> file_rows(const ResultsFile& file) {
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
  if (!start || !end) {
    return std::unexpected("a reportable file has a malformed measure line");
  }
  std::string rows;
  bool any_histogram = false;
  for (const ResultsSection& section : file.sections) {
    if (!section.name.starts_with(kHistogramPrefix)) {
      continue;
    }
    any_histogram = true;
    const std::string name = section.name.substr(kHistogramPrefix.size());
    const auto values = parse_histogram_body(section.body);
    const auto mean = mean_time(*json, name);
    if (!mean) {
      return std::unexpected("no benchmark entry for " + name);
    }
    rows += "| ";
    rows += cell(name) + " | " + cell(file.required("repetitions")) + " | " +
            cell(histogram_value(values, "batches")) + " | " + cell(*mean) + " | " +
            cell(histogram_value(values, "p50")) + " | " + cell(histogram_value(values, "p99")) +
            " | " + cell(histogram_value(values, "max")) + " | " + cell(start->load1) + " | " +
            cell(end->load1) + " | " + cell(file.required("commit").substr(0, kShortCommit)) +
            " | " + cell(file.required("claim")) + " |\n";
  }
  if (!any_histogram) {
    return std::unexpected("a results file has no histogram");
  }
  return rows;
}

}  // namespace

std::expected<std::string, std::string> render_host_table(std::span<const ResultsFile> files) {
  if (files.empty()) {
    return std::unexpected("no results files");
  }
  std::string host;
  bool any_without_claim = false;
  std::string rows;
  for (const ResultsFile& file : files) {
    if (auto admitted = admit_file(file, host); !admitted) {
      return std::unexpected(admitted.error());
    }
    any_without_claim = any_without_claim || file.required("claim") == "none";
    auto file_text = file_rows(file);
    if (!file_text) {
      return std::unexpected(file_text.error());
    }
    rows += *file_text;
  }

  std::string out = "### " + host + "\n\n";
  if (any_without_claim) {
    out += "Rows with claim `none` are harness self-tests, not performance results.\n\n";
  }
  out +=
      "| benchmark | repetitions | batches | mean batch (ns) | p50 (ns) | p99 (ns) | max (ns) | "
      "load1 start | load1 end | commit | claim |\n"
      "|---|---|---|---|---|---|---|---|---|---|---|\n";
  return out + rows;
}

std::expected<std::string, std::string> render_tables(std::span<const ResultsFile> files) {
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
    auto table = render_host_table(group);
    if (!table) {
      return std::unexpected(table.error());
    }
    out += out.empty() ? "" : "\n";
    out += *table;
  }
  return out;
}

}  // namespace aadp::results
