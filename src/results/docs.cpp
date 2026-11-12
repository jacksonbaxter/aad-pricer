#include "results/docs.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "results/aad_table.hpp"
#include "results/results_file.hpp"
#include "results/table.hpp"

namespace aadp::results {
namespace {

constexpr double kNanosecondsPerSecond = 1e9;
constexpr std::size_t kShortCommit = 7;
constexpr std::size_t kSpecFields = 11;
constexpr std::size_t kMeasuredFields = 6;
constexpr std::size_t kGroupDigits = 3;
constexpr double kGroupFrom = 1000.0;
constexpr double kGroupBelow = 1e15;
constexpr std::string_view kMeasuredRunKind = "measured_run";
constexpr std::string_view kOracleRunKind = "oracle_run";

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

// The lines of a text, without a last empty one after its final newline.
std::vector<std::string_view> lines(std::string_view text) {
  std::vector<std::string_view> out = split(text, '\n');
  if (!out.empty() && out.back().empty()) {
    out.pop_back();
  }
  return out;
}

// strtod, not std::from_chars: libc++ keeps the double overload in a dylib that older macOS
// releases lack (src/data/decimal.hpp).
std::optional<double> parse_number(std::string_view text) {
  if (text.empty() || text.size() > 64) {
    return std::nullopt;
  }
  const std::string copy(text);
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::size_t> parse_count(std::string_view text) {
  std::size_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size() || text.empty()) {
    return std::nullopt;
  }
  return value;
}

std::string format(const char* spec, double x) {
  std::array<char, 64> buffer{};
  std::snprintf(buffer.data(), buffer.size(), spec, x);
  return buffer.data();
}

// Four significant figures, with thousands separated from 1,000 on: 61,450.
std::string grouped(double x) {
  if (!(x >= kGroupFrom) || !(x < kGroupBelow)) {
    return format("%.4g", x);
  }
  const auto rounded = parse_number(format("%.3e", x));
  std::string digits = format("%.0f", rounded.value_or(x));
  for (std::size_t end = digits.size(); end > kGroupDigits; end -= kGroupDigits) {
    digits.insert(end - kGroupDigits, ",");
  }
  return digits;
}

std::string cell(std::string_view text) {
  std::string out(text);
  std::ranges::replace(out, '|', '/');
  std::ranges::replace(out, '\n', ' ');
  return out;
}

std::string short_commit(std::string_view commit) {
  return std::string(commit.substr(0, kShortCommit));
}

bool is_plain_name(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
  });
}

// A path under references/ that cannot leave it.
bool is_reference_path(std::string_view path) {
  constexpr std::string_view kPrefix = "references/";
  return path.starts_with(kPrefix) && path.ends_with(".csv") && !path.contains("..") &&
         is_plain_name(path.substr(kPrefix.size()));
}

// --- results/headline.csv ---

std::expected<void, std::string> check_headline_kind(const HeadlineRow& row,
                                                     std::string_view target,
                                                     std::string_view scale) {
  if (row.kind == kOracleRunKind) {
    if (!is_reference_path(row.source)) {
      return std::unexpected("an oracle run must name a CSV under references/");
    }
  } else if (row.target_text.empty()) {
    return std::unexpected("a BENCHMARKS.md target needs its target_text");
  }
  if (row.kind == "multiple" && split(row.detail, '/').size() != 2) {
    return std::unexpected("a multiple's detail is <variant>/<base>");
  }
  if (row.kind == "paths_per_second" && !is_plain_name(row.detail)) {
    return std::unexpected("paths_per_second's detail is one variant");
  }
  if (row.kind != kOracleRunKind && !is_plain_name(row.source)) {
    return std::unexpected("the source must be a workload or a section key");
  }
  if (row.kind != kOracleRunKind && row.better != "below" && row.better != "at_most" &&
      row.better != "at_least") {
    return std::unexpected("better must be below, at_most or at_least");
  }
  const auto scale_value = parse_number(scale);
  if (row.kind != kOracleRunKind &&
      (!parse_number(target) || !scale_value || !(*scale_value > 0.0))) {
    return std::unexpected("the target and a positive scale must be numbers");
  }
  return {};
}

std::expected<HeadlineRow, std::string> parse_spec_row(std::string_view line) {
  const std::vector<std::string_view> f = split(line, ',');
  if (f.size() != kSpecFields) {
    return std::unexpected("a row needs " + std::to_string(kSpecFields) + " fields");
  }
  HeadlineRow row{.kind = std::string(f[0]),
                  .id = std::string(f[1]),
                  .host = std::string(f[2]),
                  .source = std::string(f[3]),
                  .detail = std::string(f[4]),
                  .target_text = std::string(f[6]),
                  .better = std::string(f[7]),
                  .unit = std::string(f[9]),
                  .label = std::string(f[10])};
  constexpr std::array<std::string_view, 5> kKinds = {
      kMeasuredRunKind, "multiple", "paths_per_second", "largest", kOracleRunKind};
  if (std::ranges::find(kKinds, row.kind) == kKinds.end()) {
    return std::unexpected("unknown kind " + row.kind);
  }
  if (row.id.empty() || !is_plain_name(row.host)) {
    return std::unexpected("a row needs an id and a host label");
  }
  if (row.kind == kMeasuredRunKind) {
    const bool rest_empty =
        std::all_of(f.begin() + 4, f.end(), [](std::string_view field) { return field.empty(); });
    if (!is_reference_path(row.source) || !rest_empty) {
      return std::unexpected("a measured run names a CSV under references/ and nothing else");
    }
    return row;
  }
  if (auto checked = check_headline_kind(row, f[5], f[8]); !checked) {
    return std::unexpected(checked.error());
  }
  row.target = parse_number(f[5]).value_or(0.0);
  row.scale = parse_number(f[8]).value_or(1.0);
  return row;
}

// --- shared readers ---

// The value after `key` in a comment, up to a space, comma or full stop, if the key starts a word.
std::optional<std::string_view> token_value(std::string_view text, std::string_view key) {
  for (std::size_t at = text.find(key); at != std::string_view::npos; at = text.find(key, at + 1)) {
    if (at != 0 && text[at - 1] != ' ') {
      continue;
    }
    std::string_view rest = text.substr(at + key.size());
    rest = rest.substr(0, rest.find_first_of(" ,"));
    while (rest.ends_with('.')) {
      rest.remove_suffix(1);
    }
    return rest;
  }
  return std::nullopt;
}

std::expected<void, std::string> check_run_summary(const MeasuredRun& run,
                                                   std::string_view comment) {
  const std::size_t failed = run.rows.size() - run.passed;
  const std::array<std::pair<std::string_view, std::size_t>, 3> expected = {
      {{"run=", run.rows.size()}, {"passed=", run.passed}, {"failed=", failed}}};
  for (const auto& [key, count] : expected) {
    const auto value = token_value(comment, key);
    if (!value) {
      return std::unexpected("a summary comment has no " + std::string(key));
    }
    if (parse_count(*value) != count) {
      return std::unexpected("the summary comment says " + std::string(key) + std::string(*value) +
                             " but the rows give " + std::to_string(count));
    }
  }
  return {};
}

// The host a comment names, and the summary counts, checked against the rows.
std::expected<void, std::string> read_run_comments(MeasuredRun& run) {
  for (const std::string& comment : run.comments) {
    if (run.host.empty()) {
      run.host = std::string(token_value(comment, "Host ").value_or(""));
    }
    if (!token_value(comment, "run=")) {
      continue;
    }
    if (auto checked = check_run_summary(run, comment); !checked) {
      return std::unexpected(checked.error());
    }
  }
  return {};
}

std::expected<MeasuredRow, std::string> parse_measured_row(std::string_view line) {
  const std::vector<std::string_view> f = split(line, ',');
  if (f.size() != kMeasuredFields || f[0].empty()) {
    return std::unexpected("a row needs " + std::to_string(kMeasuredFields) + " fields and an id");
  }
  if (f[5] != "pass" && f[5] != "FAIL") {
    return std::unexpected("a verdict is pass or FAIL");
  }
  return MeasuredRow{.id = std::string(f[0]),
                     .estimate = std::string(f[1]),
                     .reference = std::string(f[2]),
                     .se = std::string(f[3]),
                     .bound = std::string(f[4]),
                     .verdict = std::string(f[5])};
}

}  // namespace

std::expected<HeadlineSpec, std::string> parse_headline_spec(std::string_view text) {
  HeadlineSpec spec;
  bool header_seen = false;
  std::size_t number = 0;
  for (const std::string_view line : lines(text)) {
    ++number;
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    if (!header_seen) {
      if (line != kHeadlineSpecHeader) {
        return std::unexpected("headline.csv: line " + std::to_string(number) +
                               " is not the header");
      }
      header_seen = true;
      continue;
    }
    auto row = parse_spec_row(line);
    if (!row) {
      return std::unexpected("headline.csv: line " + std::to_string(number) + ": " + row.error());
    }
    (row->kind == kMeasuredRunKind ? spec.measured_runs : spec.rows).push_back(std::move(*row));
  }
  if (spec.rows.empty()) {
    return std::unexpected("headline.csv: no headline rows");
  }
  for (const HeadlineRow& row : spec.rows) {
    const bool listed = std::ranges::any_of(spec.measured_runs, [&row](const HeadlineRow& run) {
      return run.source == row.source && run.host == row.host;
    });
    if (row.kind == kOracleRunKind && !listed) {
      return std::unexpected("headline.csv: " + row.source +
                             " is not a measured_run row on the same host");
    }
  }
  return spec;
}

std::expected<MeasuredRun, std::string> parse_measured_run(std::string_view text) {
  MeasuredRun run;
  bool header_seen = false;
  std::set<std::string, std::less<>> ids;
  for (const std::string_view line : lines(text)) {
    if (line.starts_with('#') && !header_seen) {
      std::string_view comment = line.substr(1);
      if (comment.starts_with(' ')) {
        comment.remove_prefix(1);
      }
      run.comments.emplace_back(comment);
    } else if (!header_seen) {
      if (line != kMeasuredRunHeader) {
        return std::unexpected("the first line after the comments is not the header");
      }
      header_seen = true;
    } else {
      auto row = parse_measured_row(line);
      if (!row) {
        return std::unexpected(row.error());
      }
      if (!ids.insert(row->id).second) {
        return std::unexpected("a repeated id: " + row->id);
      }
      if (row->verdict == "pass") {
        ++run.passed;
      }
      run.rows.push_back(std::move(*row));
    }
  }
  if (run.rows.empty()) {
    return std::unexpected("no rows");
  }
  if (auto read = read_run_comments(run); !read) {
    return std::unexpected(read.error());
  }
  return run;
}

std::expected<FaultReport, std::string> parse_fault_report(std::string_view text) {
  const std::vector<std::string_view> all = lines(text);
  if (all.empty() || all.front() != kFaultReportMagic) {
    return std::unexpected("the first line is not the fault-injection magic");
  }
  FaultReport report;
  std::size_t at = 1;
  for (; at < all.size() && !all[at].starts_with("columns: "); ++at) {
    const std::size_t colon = all[at].find(": ");
    if (colon == std::string_view::npos || colon == 0) {
      return std::unexpected("a header line is not `key: value`");
    }
    if (!report.header.emplace(all[at].substr(0, colon), all[at].substr(colon + 2)).second) {
      return std::unexpected("a repeated key: " + std::string(all[at].substr(0, colon)));
    }
  }
  if (at == all.size() || !all.back().starts_with("summary: ")) {
    return std::unexpected("no columns line, or the last line is not the summary");
  }
  report.faults = all.size() - at - 2;
  report.summary = std::string(all.back().substr(std::string_view("summary: ").size()));
  for (const std::string_view key : {"claim", "commit", "host_label", "utc_time", "selected"}) {
    if (!report.header.contains(key)) {
      return std::unexpected("the header has no " + std::string(key));
    }
  }
  return report;
}

std::optional<std::string> target_heading(std::string_view benchmarks, std::string_view id) {
  const std::string prefix = "### " + std::string(id) + ":";
  for (const std::string_view line : lines(benchmarks)) {
    if (line.starts_with(prefix)) {
      return std::string(line.substr(4));
    }
  }
  return std::nullopt;
}

std::expected<std::string, std::string> splice_headline(std::string_view readme,
                                                        std::string_view block) {
  std::size_t offset = 0;
  std::optional<std::size_t> begin_end;  // just after the begin marker's line
  std::optional<std::size_t> end_start;  // the start of the end marker's line
  for (const std::string_view line : split(readme, '\n')) {
    const bool is_begin = line == kHeadlineBegin;
    const bool is_end = line == kHeadlineEnd;
    if ((is_begin && begin_end) || (is_end && end_start) || (is_end && !begin_end)) {
      return std::unexpected("README.md: the headline markers are repeated or out of order");
    }
    if (is_begin) {
      begin_end = std::min(offset + line.size() + 1, readme.size());
    }
    if (is_end) {
      end_start = offset;
    }
    offset += line.size() + 1;
  }
  if (!begin_end || !end_start) {
    return std::unexpected("README.md: the headline markers are missing");
  }
  return std::string(readme.substr(0, *begin_end)) + std::string(block) +
         std::string(readme.substr(*end_start));
}

std::size_t first_difference_line(std::string_view a, std::string_view b) {
  if (a == b) {
    return 0;
  }
  const std::vector<std::string_view> left = split(a, '\n');
  const std::vector<std::string_view> right = split(b, '\n');
  std::size_t at = 0;
  while (at < left.size() && at < right.size() && left[at] == right[at]) {
    ++at;
  }
  return at + 1;
}

namespace {

// --- the files under results/ ---

struct ResultsEntry {
  std::string name;
  ResultsFile file;
  std::vector<AadWorkload> workloads;
};

struct FaultEntry {
  std::string name;
  FaultReport report;
};

struct HostFiles {
  std::vector<ResultsEntry> results;
  std::vector<FaultEntry> faults;
};

using Hosts = std::map<std::string, HostFiles, std::less<>>;

std::expected<void, std::string> add_file(const DocsFile& in, Hosts& hosts) {
  const std::string where = in.host_dir + "/" + in.name;
  HostFiles& host = hosts[in.host_dir];
  if (std::string_view(in.text).starts_with(kFaultReportMagic)) {
    auto report = parse_fault_report(in.text);
    if (!report) {
      return std::unexpected(where + ": " + report.error());
    }
    if (report->header.at("host_label") != in.host_dir) {
      return std::unexpected(where + " is under " + in.host_dir + " but its header says otherwise");
    }
    host.faults.push_back({.name = in.name, .report = std::move(*report)});
    return {};
  }
  auto file = parse_results_file(in.text);
  if (!file) {
    return std::unexpected(where + ": " + file.error());
  }
  if (file->required("host_label") != in.host_dir) {
    return std::unexpected(where + " is under " + in.host_dir + " but its header says otherwise");
  }
  if (auto valid = validate_stamped(*file); !valid) {
    return std::unexpected(where + ": " + valid.error());
  }
  auto workloads = read_aad_workloads(*file);
  if (!workloads) {
    return std::unexpected(where + ": " + workloads.error());
  }
  host.results.push_back(
      {.name = in.name, .file = std::move(*file), .workloads = std::move(*workloads)});
  return {};
}

// The first word of the first results file's `os`: macOS or Linux.
std::string os_name(const HostFiles& host) {
  if (host.results.empty()) {
    return {};
  }
  const std::string_view os = host.results.front().file.required("os");
  return std::string(os.substr(0, os.find_first_of(" ,")));
}

// --- headline rows ---

struct Evaluated {
  std::string target;
  std::string measured;
  std::string host;  // a link to the source, the label and the OS
  std::string verdict;
  std::string how;
  std::string commit;  // a results file's, or empty
};

std::string link(std::string_view path) {
  return "[" + std::string(path) + "](" + std::string(path) + ")";
}

std::string file_link(std::string_view host, std::string_view name) {
  return "[" + std::string(name) + "](results/" + std::string(host) + "/" + std::string(name) + ")";
}

std::string host_cell(std::string_view host, std::string_view path, const Hosts& hosts) {
  const auto found = hosts.find(host);
  const std::string os = found == hosts.end() ? std::string() : os_name(found->second);
  return "[" + std::string(host) + "](" + std::string(path) + ")" + (os.empty() ? "" : ", " + os);
}

bool names_target(std::string_view targets, std::string_view id) {
  return std::ranges::any_of(split(targets, ','), [id](std::string_view item) {
    while (item.starts_with(' ')) {
      item.remove_prefix(1);
    }
    return item.substr(0, item.find(' ')) == id;
  });
}

std::string verdict(const HeadlineRow& row, double value) {
  bool met = value >= row.target;
  if (row.better == "below") {
    met = value < row.target;
  } else if (row.better == "at_most") {
    met = value <= row.target;
  }
  std::string gap;
  if (row.kind == "paths_per_second") {
    const double ratio = row.better == "at_least" ? row.target / value : value / row.target;
    gap = format("%.1f", met ? 1.0 / ratio : ratio) + " times";
  } else {
    gap = format("%.2f", std::abs(row.target - value)) + (row.unit.empty() ? "" : " " + row.unit);
  }
  return (met ? "met, by " : "missed, by ") + gap;
}

std::string measured_text(const HeadlineRow& row, double value) {
  std::string out = row.kind == "paths_per_second" ? grouped(value) : format("%.3g", value);
  out += row.unit.empty() ? "" : " " + row.unit;
  return out + (row.label.empty() ? "" : " " + row.label);
}

struct Found {
  const ResultsEntry* entry = nullptr;
  const AadWorkload* workload = nullptr;
};

// The latest run of the workload on the host, by run_started_at. Two at the same time is refused.
std::expected<Found, std::string> find_workload(const HostFiles& host, std::string_view name) {
  Found found;
  for (const ResultsEntry& entry : host.results) {
    for (const AadWorkload& w : entry.workloads) {
      const auto field = w.fields.find("workload");
      if (field == w.fields.end() || field->second != name ||
          entry.file.required("claim") != "result") {
        continue;
      }
      const std::string_view started = entry.file.required("run_started_at");
      if (found.entry != nullptr && found.entry->file.required("run_started_at") == started) {
        return std::unexpected("two runs of " + std::string(name) + " started at " +
                               std::string(started));
      }
      if (found.entry == nullptr || found.entry->file.required("run_started_at") < started) {
        found = {.entry = &entry, .workload = &w};
      }
    }
  }
  if (found.entry == nullptr) {
    return std::unexpected("no result on this host has workload " + std::string(name));
  }
  return found;
}

std::expected<double, std::string> workload_time(const AadWorkload& w, std::string_view variant) {
  const auto time = w.times_ns.find(variant);
  if (time == w.times_ns.end()) {
    return std::unexpected("the workload has no variant " + std::string(variant));
  }
  return time->second;
}

std::expected<Evaluated, std::string> evaluate_workload(const HeadlineRow& row,
                                                        const HostFiles& host, const Hosts& hosts) {
  auto found = find_workload(host, row.source);
  if (!found) {
    return std::unexpected(found.error());
  }
  const AadWorkload& w = *found->workload;
  if (!names_target(w.fields.at("targets"), row.id)) {
    return std::unexpected(row.source + "'s targets do not name " + row.id);
  }
  const std::vector<std::string_view> parts = split(row.detail, '/');
  auto numerator = workload_time(w, parts.front());
  auto denominator = parts.size() == 2 ? workload_time(w, parts.back()) : numerator;
  if (!numerator || !denominator) {
    return std::unexpected(!numerator ? numerator.error() : denominator.error());
  }
  const std::string path = "results/" + row.host + "/" + found->entry->name;
  const std::string repetitions(found->entry->file.required("repetitions"));
  Evaluated out;
  out.host = host_cell(row.host, path, hosts);
  out.commit = short_commit(found->entry->file.required("commit"));
  double value = 0.0;
  if (row.kind == "multiple") {
    value = *numerator / *denominator / row.scale;
    out.how = "mean time of `" + std::string(parts.front()) + "` over mean time of `" +
              std::string(parts.back()) + "` on `" + row.source + "`, each the mean of " +
              repetitions + " repetitions";
  } else {
    value = w.paths * kNanosecondsPerSecond / *numerator / row.scale;
    out.how = "paths (" + format("%.0f", w.paths) + ") over mean time of `" + row.detail +
              "` on `" + row.source + "`, the mean of " + repetitions + " repetitions";
  }
  if (!w.valid) {
    out.measured = "INVALID";
    out.verdict = "INVALID: the run's in-run checks did not all pass";
    return out;
  }
  out.measured = measured_text(row, value);
  out.verdict = verdict(row, value);
  return out;
}

std::expected<Evaluated, std::string> evaluate_largest(const HeadlineRow& row,
                                                       const HostFiles& host, const Hosts& hosts) {
  const ResultsEntry* best_entry = nullptr;
  std::string best_workload;
  double best = 0.0;
  for (const ResultsEntry& entry : host.results) {
    for (const AadWorkload& w : entry.workloads) {
      const auto field = w.fields.find(row.source);
      if (!w.valid || entry.file.required("claim") != "result" || field == w.fields.end()) {
        continue;
      }
      const double value = parse_number(field->second).value_or(-1.0);
      if (value >= 0.0 && (best_entry == nullptr || value > best)) {
        best_entry = &entry;
        best = value;
        best_workload = w.fields.at("workload");
      }
    }
  }
  if (best_entry == nullptr) {
    return std::unexpected("no valid result on this host has " + row.source);
  }
  const std::string path = "results/" + row.host + "/" + best_entry->name;
  const double value = best / row.scale;
  Evaluated out;
  out.measured = measured_text(row, value) + " (`" + best_workload + "`)";
  out.host = host_cell(row.host, path, hosts);
  out.verdict = verdict(row, value);
  out.how = "the largest `" + row.source + "` of any valid AAD section on the host, divided by " +
            format("%g", row.scale);
  out.commit = short_commit(best_entry->file.required("commit"));
  return out;
}

using Runs = std::map<std::string, MeasuredRun, std::less<>>;

Evaluated evaluate_oracle_run(const HeadlineRow& row, const MeasuredRun& run, const Hosts& hosts) {
  const std::string total = std::to_string(run.rows.size());
  std::string failures;
  for (const MeasuredRow& r : run.rows) {
    if (r.verdict != "pass") {
      failures += (failures.empty() ? "" : ", ") + r.id;
    }
  }
  const std::size_t failed = run.rows.size() - run.passed;
  Evaluated out;
  out.target = "Oracle plan, " + row.id + ": all " + total +
               " registered comparisons pass the tolerance rule (`BENCHMARKS.md` section 10)";
  out.measured = std::to_string(run.passed) + " of " + total + " pass";
  out.host = host_cell(row.host, row.source, hosts);
  out.verdict = failed == 0 ? std::string("met") : std::to_string(failed) + " failed: " + failures;
  out.how = "rows with verdict `pass`, of the file's rows";
  return out;
}

std::expected<Evaluated, std::string> evaluate(const HeadlineRow& row, const Hosts& hosts,
                                               const Runs& runs, std::string_view benchmarks) {
  if (row.kind == kOracleRunKind) {
    return evaluate_oracle_run(row, runs.at(row.source), hosts);
  }
  const auto heading = target_heading(benchmarks, row.id);
  if (!heading) {
    return std::unexpected("BENCHMARKS.md has no heading for " + row.id);
  }
  if (!heading->contains(row.target_text)) {
    return std::unexpected("the heading of " + row.id + " does not say " + row.target_text);
  }
  const auto host = hosts.find(row.host);
  if (host == hosts.end()) {
    return std::unexpected("no results files for host " + row.host);
  }
  auto out = row.kind == "largest" ? evaluate_largest(row, host->second, hosts)
                                   : evaluate_workload(row, host->second, hosts);
  if (out) {
    out->target = *heading;
  }
  return out;
}

// --- rendering ---

std::string hosts_note(const std::vector<HeadlineRow>& rows, const std::vector<Evaluated>& cells,
                       const Hosts& hosts) {
  std::set<std::string, std::less<>> labels;
  std::set<std::string, std::less<>> commits;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    labels.insert(rows[i].host);
    if (!cells[i].commit.empty()) {
      commits.insert(cells[i].commit);
    }
  }
  std::string out = "Hosts:";
  std::string separator = " ";
  for (const std::string& label : labels) {
    const auto found = hosts.find(label);
    if (found == hosts.end() || found->second.results.empty()) {
      continue;
    }
    const ResultsFile& file = found->second.results.front().file;
    out += separator;
    out +=
        "`" + label + "` is " + cell(file.required("cpu_model")) + " on " + os_name(found->second);
    separator = "; ";
  }
  out +=
      ". Each row's host cell links its source file. A results file's header records the "
      "pinning, compiler and flags.";
  if (commits.empty()) {
    return out + "\n";
  }
  out += commits.size() == 1 ? " Benchmark rows were measured at commit"
                             : " Benchmark rows were measured at commits";
  separator = " ";
  for (const std::string& commit : commits) {
    out += separator;
    out += "`" + commit + "`";
    separator = ", ";
  }
  return out + ".\n";
}

std::string headline_table(const std::vector<Evaluated>& cells, bool sources) {
  std::string out = sources ? "| target | measured | host | verdict | computed as |\n"
                              "|---|---|---|---|---|\n"
                            : "| target | measured | host | verdict |\n|---|---|---|---|\n";
  for (const Evaluated& e : cells) {
    out += "| " + cell(e.target) + " | " + cell(e.measured) + " | " + e.host + " | " +
           cell(e.verdict) + (sources ? " | " + cell(e.how) : std::string()) + " |\n";
  }
  return out;
}

std::string run_value(const MeasuredRun& run, const std::string& id) {
  const auto row = std::ranges::find(run.rows, id, &MeasuredRow::id);
  if (row == run.rows.end()) {
    return "| - | - | - | - | not run ";
  }
  return "| " + row->estimate + " | " + row->reference + " | " + row->se + " | " + row->bound +
         " | " + row->verdict + " ";
}

std::string measured_runs_section(const HeadlineSpec& spec, const Runs& runs, const Hosts& hosts) {
  std::string out =
      "## Oracle plan: measured runs\n\n"
      "Each run judges the registered comparisons of `references/comparison_plan.csv` by the "
      "tolerance rule of `BENCHMARKS.md` section 10: bound = b + critical x SE. The runs stand "
      "side by side; none replaces another.\n\n"
      "| run | file | host | rows | passed | failed |\n|---|---|---|---|---|---|\n";
  for (const HeadlineRow& row : spec.measured_runs) {
    const MeasuredRun& run = runs.at(row.source);
    const auto host = hosts.find(row.host);
    const std::string os = host == hosts.end() ? std::string() : os_name(host->second);
    out += "| " + cell(row.id) + " | " + link(row.source) + " | " + row.host +
           (os.empty() ? "" : ", " + os) + " | " + std::to_string(run.rows.size()) + " | " +
           std::to_string(run.passed) + " | " + std::to_string(run.rows.size() - run.passed) +
           " |\n";
  }
  // An indented code block, so the header's lines and symbols reach the reader as written.
  for (const HeadlineRow& row : spec.measured_runs) {
    out += "\nThe header of " + cell(row.id) + ", `" + row.source + "`:\n\n";
    for (const std::string& comment : runs.at(row.source).comments) {
      out += "    " + comment + "\n";
    }
  }
  // Every id once, in the order of the last run that has it first.
  std::vector<std::string> ids;
  std::set<std::string, std::less<>> seen;
  for (const HeadlineRow& row : spec.measured_runs | std::views::reverse) {
    for (const MeasuredRow& r : runs.at(row.source).rows) {
      if (seen.insert(r.id).second) {
        ids.push_back(r.id);
      }
    }
  }
  out += "\n| id";
  std::string rule = "|---";
  for (const HeadlineRow& row : spec.measured_runs) {
    for (const std::string_view column : {"estimate", "reference", "SE", "bound", "verdict"}) {
      out += " | " + cell(row.id) + " " + std::string(column);
      rule += "|---";
    }
  }
  out += " |\n" + rule + "|\n";
  for (const std::string& id : ids) {
    out += "| " + cell(id) + " ";
    for (const HeadlineRow& row : spec.measured_runs) {
      out += run_value(runs.at(row.source), id);
    }
    out += "|\n";
  }
  return out;
}

std::string files_table(std::string_view label, const HostFiles& host) {
  std::string out =
      "### Files\n\n| file | claim | commit | started (UTC) | OS | CPU | pinning |\n"
      "|---|---|---|---|---|---|---|\n";
  for (const ResultsEntry& e : host.results) {
    const ResultsFile& f = e.file;
    out += "| " + file_link(label, e.name) + " | " + cell(f.required("claim")) + " | `" +
           short_commit(f.required("commit")) + "` | " + cell(f.required("run_started_at")) +
           " | " + cell(f.required("os")) + " | " + cell(f.required("cpu_model")) + " | " +
           cell(f.required("pinning_state")) + ", CPUs " + cell(f.required("pinned_cpus")) + " |\n";
  }
  if (host.faults.empty()) {
    return out;
  }
  out +=
      "\n### Fault-injection reports\n\nEach plants one bug at a time and records whether its "
      "check caught it. The manifest has grown since; `docs/NOTES.md` has the later runs.\n\n"
      "| file | commit | time (UTC) | faults run | scope | summary |\n|---|---|---|---|---|---|\n";
  for (const FaultEntry& e : host.faults) {
    const auto& h = e.report.header;
    const auto scope = h.find("scope");
    out += "| " + file_link(label, e.name) + " | `" + short_commit(h.at("commit")) + "` | " +
           cell(h.at("utc_time")) + " | " + std::to_string(e.report.faults) + " | " +
           (scope == h.end() ? std::string("absent") : cell(scope->second)) + " | " +
           cell(e.report.summary) + " |\n";
  }
  return out;
}

std::expected<std::string, std::string> host_section(std::string_view label,
                                                     const HostFiles& host) {
  std::string out = "\n## " + std::string(label) + "\n\n" + files_table(label, host);
  if (host.results.empty()) {
    return out;
  }
  std::vector<ResultsFile> files;
  files.reserve(host.results.size());
  for (const ResultsEntry& e : host.results) {
    files.push_back(e.file);
  }
  auto table = render_host_table(files, {.iteration = false, .host_heading = false});
  if (!table) {
    return std::unexpected(std::string(label) + ": " + table.error());
  }
  return out + "\n### Benchmarks\n\n" + *table;
}

constexpr std::string_view kResultsIntro =
    "<!-- Generated by `aadp_results_table --docs . --write` from results/ and references/. "
    "Do not edit: scripts/check.sh fails if this file differs from a fresh generation. -->\n"
    "\n# Results\n\n"
    "Every number here comes from a committed file: the results files in `results/<host>/`, "
    "each stamped by `aadp_results_stamp`, and the measured oracle runs in `references/`. "
    "No table holds two hosts. The targets and their sources are in "
    "[BENCHMARKS.md](BENCHMARKS.md), and the findings behind the numbers in "
    "[docs/NOTES.md](docs/NOTES.md).\n\n"
    "## Headline\n\n"
    "The README's headline rows, chosen in `results/headline.csv`, with how each value is "
    "computed. A row's target is its heading in `BENCHMARKS.md`.\n\n";

}  // namespace

std::expected<Docs, std::string> render_docs(const DocsInputs& inputs) {
  auto spec = parse_headline_spec(inputs.spec);
  if (!spec) {
    return std::unexpected(spec.error());
  }
  Runs runs;
  for (const HeadlineRow& row : spec->measured_runs) {
    const auto text = inputs.measured_runs.find(row.source);
    if (text == inputs.measured_runs.end()) {
      return std::unexpected(row.source + ": not read");
    }
    auto run = parse_measured_run(text->second);
    if (!run) {
      return std::unexpected(row.source + ": " + run.error());
    }
    if (!run->host.empty() && run->host != row.host) {
      return std::unexpected(row.source + " names host " + run->host + ", not " + row.host);
    }
    runs.emplace(row.source, std::move(*run));
  }
  Hosts hosts;
  for (const DocsFile& file : inputs.files) {
    if (auto added = add_file(file, hosts); !added) {
      return std::unexpected(added.error());
    }
  }
  for (auto& [label, host] : hosts) {
    std::ranges::sort(host.results, {}, &ResultsEntry::name);
    std::ranges::sort(host.faults, {}, &FaultEntry::name);
  }
  std::vector<Evaluated> cells;
  for (const HeadlineRow& row : spec->rows) {
    auto evaluated = evaluate(row, hosts, runs, inputs.benchmarks);
    if (!evaluated) {
      return std::unexpected("headline " + row.id + " on " + row.host + ": " + evaluated.error());
    }
    cells.push_back(std::move(*evaluated));
  }
  const std::string note = hosts_note(spec->rows, cells, hosts);
  Docs docs{.headline = headline_table(cells, false) + "\n" + note,
            .results_md = std::string(kResultsIntro) + headline_table(cells, true) + "\n" + note +
                          "\n" + measured_runs_section(*spec, runs, hosts)};
  for (const auto& [label, host] : hosts) {
    auto section = host_section(label, host);
    if (!section) {
      return std::unexpected(section.error());
    }
    docs.results_md += *section;
  }
  return docs;
}

}  // namespace aadp::results
