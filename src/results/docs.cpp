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
#include "results/report.hpp"
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
constexpr std::string_view kReportCellKind = "report_cell";

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

bool is_workload_kind(std::string_view kind) {
  return kind == "multiple" || kind == "paths_per_second";
}

// A workload row's source: the workload, then optionally `@` and a build's simd_cmake_target.
// The build picks one preset's files where a host ran the workload under two (linux-zen1 runs
// the first-order workloads in its release and release-avx2 presets).
struct WorkloadSource {
  std::string_view workload;
  std::string_view build;  // empty: every build
};

WorkloadSource workload_source(std::string_view source) {
  const std::size_t at = source.find('@');
  if (at == std::string_view::npos) {
    return {.workload = source, .build = {}};
  }
  return {.workload = source.substr(0, at), .build = source.substr(at + 1)};
}

bool is_plain_source(const HeadlineRow& row) {
  if (!is_workload_kind(row.kind) || !row.source.contains('@')) {
    return is_plain_name(row.source);
  }
  const WorkloadSource source = workload_source(row.source);
  return is_plain_name(source.workload) && is_plain_name(source.build);
}

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
  if (row.kind == kReportCellKind) {
    const std::vector<std::string_view> parts = split(row.detail, '/');
    if (parts.size() != 2 || parts.front().empty() || parts.back().empty()) {
      return std::unexpected("a report cell's detail is <row key>/<column>");
    }
  }
  if (row.kind != kOracleRunKind && !is_plain_source(row)) {
    return std::unexpected("the source must be a workload, workload@build, or a section key");
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
  constexpr std::array<std::string_view, 6> kKinds = {
      kMeasuredRunKind, "multiple", "paths_per_second", "largest", kOracleRunKind, kReportCellKind};
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

bool is_commit_text(std::string_view text) {
  constexpr std::size_t kShortest = 7;
  constexpr std::size_t kLongest = 40;
  return text.size() >= kShortest && text.size() <= kLongest &&
         std::ranges::all_of(
             text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// The host and the commit a comment names, and the summary counts, checked against the rows.
std::expected<void, std::string> read_run_comments(MeasuredRun& run) {
  for (const std::string& comment : run.comments) {
    if (run.host.empty()) {
      run.host = std::string(token_value(comment, "Host ").value_or(""));
    }
    if (const auto commit = token_value(comment, "run at commit "); commit && run.commit.empty()) {
      if (!is_commit_text(*commit)) {
        return std::unexpected("a `run at commit` comment does not name a commit");
      }
      run.commit = std::string(*commit);
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

// --- results/verdicts.csv and BENCHMARKS.md's "Measured" lines ---

constexpr std::size_t kVerdictFieldsBeforeNote = 9;
constexpr std::array<std::string_view, 2> kHostLabels = {"linux-zen1", "mac-m4max"};

bool all_in(std::string_view text, char low, char high) {
  return !text.empty() &&
         std::ranges::all_of(text, [low, high](char c) { return c >= low && c <= high; });
}

// AAD-1, SVI-11: capitals, a hyphen and digits; or a code-quality bar item, B34.
bool is_target_id(std::string_view id) {
  const std::size_t hyphen = id.find('-');
  if (hyphen == std::string_view::npos) {
    return id.starts_with('B') && all_in(id.substr(1), '0', '9');
  }
  return all_in(id.substr(0, hyphen), 'A', 'Z') && all_in(id.substr(hyphen + 1), '0', '9');
}

bool is_known_host(std::string_view host) {
  return host == kAnyHost || std::ranges::find(kHostLabels, host) != kHostLabels.end();
}

// GitHub's anchor for a heading: lower case, spaces to hyphens, every other character but
// letters, digits, hyphens and underscores dropped. Empty for a heading with a byte outside
// ASCII, whose anchor that rule does not settle.
std::string anchor_of(std::string_view heading) {
  std::string out;
  for (const char c : heading) {
    if (static_cast<unsigned char>(c) >= 0x80U) {
      return {};
    }
    if (c >= 'A' && c <= 'Z') {
      out += static_cast<char>(c - 'A' + 'a');
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
      out += c;
    } else if (c == ' ') {
      out += '-';
    }
  }
  return out;
}

bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Sets met or missed for each verdict word in the text.
void read_verdict_words(std::string_view text, bool& met, bool& missed) {
  std::size_t at = 0;
  while (at < text.size()) {
    if (!is_letter(text[at])) {
      ++at;
      continue;
    }
    std::size_t end = at;
    std::string word;
    for (; end < text.size() && is_letter(text[end]); ++end) {
      word += ascii_lower(text[end]);
    }
    met = met || word == "met" || word == "meets" || word == "meeting";
    missed = missed || word == "missed" || word == "misses" || word == "missing";
    at = end;
  }
}

// "- B34. ..." at the top level: the bar item's id.
std::optional<std::string> bar_item(std::string_view line) {
  if (!line.starts_with("- B")) {
    return std::nullopt;
  }
  const std::size_t dot = line.find(". ");
  if (dot == std::string_view::npos || !is_target_id(line.substr(2, dot - 2))) {
    return std::nullopt;
  }
  return std::string(line.substr(2, dot - 2));
}

// A heading's text, without its hashes and the spaces after them.
std::string_view heading_text(std::string_view line) {
  while (line.starts_with('#')) {
    line.remove_prefix(1);
  }
  while (line.starts_with(' ')) {
    line.remove_prefix(1);
  }
  return line;
}

// A "Measured" or "Waiting" line's hosts and verdict words; `body` starts at either word.
MeasuredLine read_measured_line(std::string_view body, std::size_t number,
                                const std::string& target, const std::string& anchor) {
  MeasuredLine measured{.line = number,
                        .target = target,
                        .anchor = anchor,
                        .hosts = {},
                        .met = false,
                        .missed = false,
                        .not_counted = false,
                        .waiting = body.starts_with("Waiting")};
  for (const std::string_view host : kHostLabels) {
    if (body.contains("`" + std::string(host) + "`")) {
      measured.hosts.emplace_back(host);
    }
  }
  if (measured.waiting) {
    return measured;  // a step that has not run states no verdict
  }
  std::string lower(body);
  std::ranges::transform(lower, lower.begin(), ascii_lower);
  // A verdict not counted: its verdict words are the outcomes of its runs.
  measured.not_counted = lower.contains("not counted");
  if (!measured.not_counted) {
    read_verdict_words(body, measured.met, measured.missed);
  }
  return measured;
}

// results/<host>/<name>.txt, the name of letters, digits, '-', '_' and '.', and no "..": a
// stamped file's name holds its UTC time, 20261117T070836Z.
bool is_results_path(std::string_view path, std::string_view host) {
  const std::string prefix = "results/" + std::string(host) + "/";
  if (!path.starts_with(prefix) || !path.ends_with(".txt") || path.contains("..")) {
    return false;
  }
  const std::string_view name = path.substr(prefix.size());
  return std::ranges::all_of(name, [](char c) {
    return is_plain_name(std::string_view(&c, 1)) || (c >= 'A' && c <= 'Z');
  });
}

bool is_counted(const VerdictRow& row) { return row.verdict != "split" && row.verdict != "marked"; }

bool is_waiting(const VerdictRow& row) { return row.verdict == "waiting"; }

// "met 1.05", "missed 1.12 times": a run's outcome, then its figure. Empty if `run` is empty.
std::expected<std::string_view, std::string> run_outcome(std::string_view run) {
  if (run.empty()) {
    return std::string_view();
  }
  const std::size_t space = run.find(' ');
  const std::string_view outcome = run.substr(0, space);
  if (space == std::string_view::npos || space + 1 == run.size() ||
      (outcome != "met" && outcome != "missed")) {
    return std::unexpected(std::string("a run is \"<met|missed> <figure>\": ") + std::string(run));
  }
  return outcome;
}

// The runs a verdict carries: split, both, with different outcomes; marked, its rerun; met or
// missed, both or neither, each the verdict; none, neither.
std::expected<void, std::string> check_verdict_runs(const VerdictRow& row) {
  const auto withdrawn = run_outcome(row.withdrawn);
  const auto rerun = run_outcome(row.rerun);
  if (!withdrawn || !rerun) {
    return std::unexpected(withdrawn ? rerun.error() : withdrawn.error());
  }
  if (is_waiting(row)) {
    if (!withdrawn->empty() || !rerun->empty()) {
      return std::unexpected("a waiting row gives no run: its step has not run");
    }
  } else if (row.verdict == "split") {
    if (withdrawn->empty() || rerun->empty() || *withdrawn == *rerun) {
      return std::unexpected(
          "a split row gives the withdrawn run and the rerun, with different outcomes");
    }
  } else if (row.verdict == "marked") {
    if (rerun->empty()) {
      return std::unexpected("a marked row gives the figure it publishes, as its rerun");
    }
  } else if (withdrawn->empty() != rerun->empty() ||
             (!withdrawn->empty() && (*withdrawn != row.verdict || *rerun != row.verdict))) {
    return std::unexpected("a " + row.verdict + " row gives both runs or neither, each " +
                           row.verdict + "; runs that differ make it split");
  }
  if ((!is_counted(row) || is_waiting(row)) && !row.headline.empty()) {
    return std::unexpected("a verdict not counted names no headline row");
  }
  return {};
}

// files cites results files and test none; pending, a waiting row's alone, cites none.
std::expected<void, std::string> check_verdict_basis(const VerdictRow& row) {
  if (row.basis != "files" && row.basis != "test" && row.basis != "pending") {
    return std::unexpected("a basis is files, test or pending");
  }
  if (is_waiting(row) != (row.basis == "pending")) {
    return std::unexpected("a waiting row's basis is pending, and only a waiting row's");
  }
  if ((row.basis == "files") == row.files.empty() ||
      (row.host == kAnyHost && row.basis != "test")) {
    return std::unexpected(
        "a files row cites files and a test or pending row none; a row on any host is a test");
  }
  return {};
}

std::expected<void, std::string> check_verdict_fields(const VerdictRow& row) {
  if (!is_target_id(row.target) || !is_known_host(row.host)) {
    return std::unexpected("a row needs a target id and a host label, or `any`");
  }
  if (row.verdict != "met" && row.verdict != "missed" && row.verdict != "none" &&
      !is_waiting(row) && is_counted(row)) {
    return std::unexpected("a verdict is met, missed, none, split, marked or waiting");
  }
  if (auto runs = check_verdict_runs(row); !runs) {
    return runs;
  }
  if (auto basis = check_verdict_basis(row); !basis) {
    return basis;
  }
  for (const std::string& file : row.files) {
    if (!is_results_path(file, row.host)) {
      return std::unexpected(file + " is not results/" + row.host + "/<name>.txt");
    }
  }
  const std::vector<std::string_view> headline = split(row.headline, ' ');
  if (!row.headline.empty() && (row.basis != "files" || headline.size() > 2 ||
                                std::ranges::any_of(headline, &std::string_view::empty))) {
    return std::unexpected(R"(a headline is "<source> <detail>" or "<source>", on a files row)");
  }
  if (row.condition.contains('|') || row.withdrawn.contains('|') || row.rerun.contains('|') ||
      row.note.contains('|')) {
    return std::unexpected("a condition, run or note holds a |");
  }
  return {};
}

std::expected<VerdictRow, std::string> parse_verdict_row(std::string_view line) {
  std::array<std::string_view, kVerdictFieldsBeforeNote> f{};
  for (std::string_view& field : f) {
    const std::size_t comma = line.find(',');
    if (comma == std::string_view::npos) {
      return std::unexpected("a row needs 10 fields, the note last");
    }
    field = line.substr(0, comma);
    line.remove_prefix(comma + 1);
  }
  VerdictRow row{.target = std::string(f[0]),
                 .host = std::string(f[1]),
                 .condition = std::string(f[2]),
                 .verdict = std::string(f[3]),
                 .basis = std::string(f[4]),
                 .files = {},
                 .headline = std::string(f[6]),
                 .withdrawn = std::string(f[7]),
                 .rerun = std::string(f[8]),
                 .note = std::string(line)};
  if (!f[5].empty()) {
    for (const std::string_view file : split(f[5], ' ')) {
      row.files.emplace_back(file);
    }
  }
  if (auto checked = check_verdict_fields(row); !checked) {
    return std::unexpected(checked.error());
  }
  return row;
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

std::expected<std::vector<VerdictRow>, std::string> parse_verdicts(std::string_view text) {
  std::vector<VerdictRow> rows;
  bool header_seen = false;
  std::size_t number = 0;
  for (const std::string_view line : lines(text)) {
    ++number;
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    const std::string where = "verdicts.csv: line " + std::to_string(number);
    if (!header_seen) {
      if (line != kVerdictsHeader) {
        return std::unexpected(where + " is not the header");
      }
      header_seen = true;
      continue;
    }
    auto row = parse_verdict_row(line);
    if (!row) {
      return std::unexpected(where + ": " + row.error());
    }
    for (const VerdictRow& other : rows) {
      if (other.target == row->target && other.host == row->host &&
          (other.condition == row->condition || other.condition.empty() ||
           row->condition.empty())) {
        return std::unexpected(where + ": " + row->target + " on " + row->host +
                               " has two rows of one condition, or an empty condition beside "
                               "another row");
      }
    }
    rows.push_back(std::move(*row));
  }
  if (rows.empty()) {
    return std::unexpected("verdicts.csv: no rows");
  }
  return rows;
}

std::vector<MeasuredLine> measured_lines(std::string_view benchmarks) {
  std::vector<MeasuredLine> out;
  std::string target;
  std::string anchor;
  std::string heading_anchor;
  bool in_target_section = false;
  std::size_t number = 0;
  for (const std::string_view line : lines(benchmarks)) {
    ++number;
    if (line.starts_with('#')) {
      const std::string_view text = heading_text(line);
      heading_anchor = anchor_of(text);
      const std::size_t colon = text.find(':');
      in_target_section = line.starts_with("### ") && colon != std::string_view::npos &&
                          is_target_id(text.substr(0, colon));
      target = in_target_section ? std::string(text.substr(0, colon)) : std::string();
      anchor = heading_anchor;
      continue;
    }
    // Outside a target's section, a top-level `- B<n>.` opens a bar item, and any other
    // top-level line closes it.
    if (!in_target_section && !line.empty() && !line.starts_with(' ')) {
      target = bar_item(line).value_or("");
      anchor = heading_anchor;
    }
    std::string_view body = line;
    while (body.starts_with(' ')) {
      body.remove_prefix(1);
    }
    if (!target.empty() && (body.starts_with("- Measured") || body.starts_with("- Waiting"))) {
      body.remove_prefix(2);
      out.push_back(read_measured_line(body, number, target, anchor));
    }
  }
  return out;
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
  std::vector<ReportTable> reports;  // A report file's tables; empty for a benchmark file
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
  std::vector<ReportTable> reports;
  if (file_kind(*file) == FileKind::kReport) {
    auto tables = read_report(*file);
    if (!tables) {
      return std::unexpected(where + ": " + tables.error().detail);
    }
    reports = std::move(*tables);
  }
  host.results.push_back({.name = in.name,
                          .file = std::move(*file),
                          .workloads = std::move(*workloads),
                          .reports = std::move(reports)});
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
  std::string commit;       // the source file's, short
  bool uses_chain = false;  // a report_cell row whose section is derived from the option chain
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

// The latest run of the workload on the host, by run_started_at, in the source's build if it
// names one. Two at the same time is refused.
std::expected<Found, std::string> find_workload(const HostFiles& host, std::string_view source) {
  const auto [name, build] = workload_source(source);
  Found found;
  for (const ResultsEntry& entry : host.results) {
    for (const AadWorkload& w : entry.workloads) {
      const auto field = w.fields.find("workload");
      if (field == w.fields.end() || field->second != name ||
          entry.file.required("claim") != "result" ||
          (!build.empty() && entry.file.required("simd_cmake_target") != build)) {
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
    return std::unexpected("no result on this host has workload " + std::string(name) +
                           (build.empty() ? "" : " in build " + std::string(build)));
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
    return std::unexpected(std::string(workload_source(row.source).workload) +
                           "'s targets do not name " + row.id);
  }
  const std::vector<std::string_view> parts = split(row.detail, '/');
  auto numerator = workload_time(w, parts.front());
  auto denominator = parts.size() == 2 ? workload_time(w, parts.back()) : numerator;
  if (!numerator || !denominator) {
    return std::unexpected(!numerator ? numerator.error() : denominator.error());
  }
  const std::string path = "results/" + row.host + "/" + found->entry->name;
  const ResultsFile& file = found->entry->file;
  const std::string repetitions(file.required("repetitions"));
  const auto [name, build] = workload_source(row.source);
  const std::string on = "`" + std::string(name) + "`" +
                         (build.empty() ? std::string()
                                        : " in the `" + std::string(build) + "` build (" +
                                              std::string(file.required("simd_target")) + ")");
  Evaluated out;
  out.host = host_cell(row.host, path, hosts);
  out.commit = short_commit(file.required("commit"));
  double value = 0.0;
  if (row.kind == "multiple") {
    value = *numerator / *denominator / row.scale;
    out.how = "mean time of `" + std::string(parts.front()) + "` over mean time of `" +
              std::string(parts.back()) + "` on " + on + ", each the mean of " + repetitions +
              " repetitions";
  } else {
    value = w.paths * kNanosecondsPerSecond / *numerator / row.scale;
    out.how = "paths (" + format("%.0f", w.paths) + ") over mean time of `" + row.detail + "` on " +
              on + ", the mean of " + repetitions + " repetitions";
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

struct FoundCell {
  const ResultsEntry* entry = nullptr;
  const ReportTable* table = nullptr;
};

// The latest run on the host, by run_started_at, of a result that has report section `name`.
// Two at that latest time is refused, as for a workload; a tie between earlier runs is not.
std::expected<FoundCell, std::string> find_report(const HostFiles& host, std::string_view name) {
  FoundCell found;
  bool tied = false;
  for (const ResultsEntry& entry : host.results) {
    const auto table = std::ranges::find(entry.reports, name, &ReportTable::name);
    if (table == entry.reports.end() || entry.file.required("claim") != "result") {
      continue;
    }
    const std::string_view started = entry.file.required("run_started_at");
    if (found.entry != nullptr && found.entry->file.required("run_started_at") == started) {
      tied = true;
    } else if (found.entry == nullptr || found.entry->file.required("run_started_at") < started) {
      found = {.entry = &entry, .table = &*table};
      tied = false;
    }
  }
  if (found.entry == nullptr) {
    return std::unexpected("no result on this host has report section " + std::string(name));
  }
  if (tied) {
    return std::unexpected("two runs of report section " + std::string(name) + " started at " +
                           std::string(found.entry->file.required("run_started_at")));
  }
  return found;
}

// One number cell of one report section: detail is "<row key>/<column>".
std::expected<Evaluated, std::string> evaluate_report_cell(const HeadlineRow& row,
                                                           const HostFiles& host,
                                                           const Hosts& hosts) {
  auto found = find_report(host, row.source);
  if (!found) {
    return std::unexpected(found.error());
  }
  const ReportTable& table = *found->table;
  if (std::ranges::find(table.targets, row.id) == table.targets.end()) {
    return std::unexpected("report section " + row.source + "'s targets do not name " + row.id);
  }
  const std::vector<std::string_view> parts = split(row.detail, '/');
  const ReportRow* entry = table.row(parts.front());
  const auto column = table.column(parts.back());
  if (entry == nullptr || !column) {
    return std::unexpected("report section " + row.source + " has no cell " + row.detail);
  }
  const auto number = read_number_cell(table.columns[*column], entry->cells[*column]);
  if (!number) {
    return std::unexpected("report cell " + row.detail + " of " + row.source + " is not a number");
  }
  const double value = *number / row.scale;
  const std::string path = "results/" + row.host + "/" + found->entry->name;
  Evaluated out;
  out.measured = measured_text(row, value);
  out.host = host_cell(row.host, path, hosts);
  out.verdict = verdict(row, value);
  out.how = "`" + std::string(parts.back()) + "` of row `" + std::string(parts.front()) +
            "` in report section `" + row.source + "`" + (table.timing ? ", a timing," : "") +
            " of the latest result on the host" +
            (row.scale == 1.0 ? std::string() : ", divided by " + format("%g", row.scale));
  out.commit = short_commit(found->entry->file.required("commit"));
  out.uses_chain = table.uses_chain;
  return out;
}

using Runs = std::map<std::string, MeasuredRun, std::less<>>;

std::expected<Evaluated, std::string> evaluate_oracle_run(const HeadlineRow& row,
                                                          const MeasuredRun& run,
                                                          const Hosts& hosts) {
  if (run.commit.empty()) {
    return std::unexpected(row.source + " names no commit it ran at (a `run at commit` comment)");
  }
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
  out.commit = short_commit(run.commit);
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
  std::expected<Evaluated, std::string> out;
  if (row.kind == "largest") {
    out = evaluate_largest(row, host->second, hosts);
  } else if (row.kind == kReportCellKind) {
    out = evaluate_report_cell(row, host->second, hosts);
  } else {
    out = evaluate_workload(row, host->second, hosts);
  }
  if (out) {
    out->target = *heading;
  }
  return out;
}

// --- rendering ---

// "a", "a and b", "a, b and c".
std::string joined(const std::vector<std::string>& items) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += i + 1 == items.size() ? " and " : ", ";
    }
    out += items[i];
  }
  return out;
}

// Which rows were measured at which commit, each commit from the row's own source file, in the
// order the rows first name them.
std::string commits_note(const std::vector<HeadlineRow>& rows,
                         const std::vector<Evaluated>& cells) {
  std::vector<std::pair<std::string, std::vector<std::string>>> commits;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const std::string name =
        rows[i].kind == kOracleRunKind ? "the oracle plan's " + rows[i].id : rows[i].id;
    auto at = std::ranges::find(commits, cells[i].commit,
                                &std::pair<std::string, std::vector<std::string>>::first);
    if (at == commits.end()) {
      commits.push_back({cells[i].commit, {}});
      at = commits.end() - 1;
    }
    if (std::ranges::find(at->second, name) == at->second.end()) {
      at->second.push_back(name);
    }
  }
  if (commits.size() == 1) {
    return " Every row was measured at commit `" + commits.front().first +
           "`, as its source file says.";
  }
  std::string out = " Each row's commit, as its source file says:";
  std::string_view separator = " ";
  for (const auto& [commit, names] : commits) {
    out += separator;
    out += '`';
    out += commit;
    out += "` for ";
    out += joined(names);
    separator = "; ";
  }
  return out + ".";
}

std::string hosts_note(const std::vector<HeadlineRow>& rows, const std::vector<Evaluated>& cells,
                       const Hosts& hosts) {
  std::set<std::string, std::less<>> labels;
  bool any_chain = false;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    labels.insert(rows[i].host);
    any_chain = any_chain || cells[i].uses_chain;
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
  if (any_chain) {
    out +=
        " A row from a report section derived from the option chain keeps the chain's terms; "
        "the attribution is in [NOTICE](NOTICE) and beside the section in RESULTS.md.";
  }
  return out + commits_note(rows, cells) + "\n";
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
  std::vector<ResultsFile> benchmarks;
  std::vector<ResultsFile> reports;
  for (const ResultsEntry& e : host.results) {
    (e.reports.empty() ? benchmarks : reports).push_back(e.file);
  }
  const TableOptions options{.iteration = false, .host_heading = false};
  if (!benchmarks.empty()) {
    auto table = render_host_table(benchmarks, options);
    if (!table) {
      return std::unexpected(std::string(label) + ": " + table.error());
    }
    out +=
        "\n### Benchmarks\n\nA row's build is its file's `simd_cmake_target` and instruction set "
        "(`simd_target`): the `release` preset builds `baseline` and `release-avx2` builds `avx2` "
        "(`CMakePresets.json`).\n\n" +
        *table;
  }
  if (!reports.empty()) {
    auto tables = render_report_tables(reports, options);
    if (!tables) {
      return std::unexpected(std::string(label) + ": " + tables.error());
    }
    out += "\n### Reports\n\n" + *tables;
  }
  return out;
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

// --- results/verdicts.csv against BENCHMARKS.md, the headline and the files ---

using TargetHost = std::pair<std::string, std::string>;

struct Stated {
  bool met = false;
  bool missed = false;
  bool none = false;
  bool not_counted = false;
  bool waiting = false;
  bool silent = false;   // a "Measured" line states no verdict at all
  std::size_t line = 0;  // BENCHMARKS.md's first line for the pair
};

std::string target_on(const TargetHost& key) { return key.first + " on " + key.second; }

// Whether the "Measured" lines of a target and host state the row's verdict.
bool states(const Stated& line, const VerdictRow& row) {
  if (row.verdict == "met") {
    return line.met;
  }
  if (row.verdict == "missed") {
    return line.missed;
  }
  if (is_waiting(row)) {
    return line.waiting;
  }
  return is_counted(row) ? line.silent : line.not_counted;
}

// What the "Measured" lines state, by target and host.
std::map<TargetHost, Stated> stated_by_lines(const std::vector<MeasuredLine>& lines) {
  std::map<TargetHost, Stated> stated;
  for (const MeasuredLine& m : lines) {
    std::vector<std::string> on = m.hosts;
    if (on.empty() && (m.met || m.missed || m.not_counted || m.waiting)) {
      on.emplace_back(kAnyHost);
    }
    for (const std::string& host : on) {
      Stated& s = stated[{m.target, host}];
      s.line = s.line == 0 ? m.line : s.line;
      s.met = s.met || m.met;
      s.missed = s.missed || m.missed;
      s.not_counted = s.not_counted || m.not_counted;
      s.waiting = s.waiting || m.waiting;
      s.silent = s.silent || (!m.met && !m.missed && !m.not_counted && !m.waiting);
    }
  }
  return stated;
}

// What the rows give, by target and host.
std::map<TargetHost, Stated> given_by_rows(const std::vector<VerdictRow>& rows) {
  std::map<TargetHost, Stated> given;
  for (const VerdictRow& row : rows) {
    Stated& g = given[{row.target, row.host}];
    g.met = g.met || row.verdict == "met";
    g.missed = g.missed || row.verdict == "missed";
    g.none = g.none || row.verdict == "none";
    g.not_counted = g.not_counted || !is_counted(row);
    g.waiting = g.waiting || is_waiting(row);
  }
  return given;
}

// Why the rows `g` of a target and host leave out what its lines `s` state; empty if they do not.
std::string left_out(const TargetHost& key, const Stated& s, const Stated& g) {
  const std::string at = "BENCHMARKS.md line " + std::to_string(s.line) + " and after: ";
  if (s.met && !g.met) {
    return at + target_on(key) + " is stated met, and results/verdicts.csv has no met row for it";
  }
  if (s.missed && !g.missed) {
    return at + target_on(key) +
           " is stated missed, and results/verdicts.csv has no missed row for it";
  }
  if (s.not_counted && !g.not_counted) {
    return at + target_on(key) +
           " is stated not counted, and results/verdicts.csv has no split or marked row for it";
  }
  if (s.silent && !s.met && !s.missed && !s.not_counted && !g.none) {
    return at + target_on(key) +
           " is measured with no verdict, and results/verdicts.csv has no none row for it";
  }
  if (s.waiting && !g.waiting) {
    return at + target_on(key) +
           " is stated waiting, and results/verdicts.csv has no waiting row for it";
  }
  return {};
}

// Every target, host and verdict a "Measured" line states has a row, and every row has a line
// that states it.
std::expected<void, std::string> check_against_benchmarks(const std::vector<VerdictRow>& rows,
                                                          const std::vector<MeasuredLine>& lines) {
  const std::map<TargetHost, Stated> stated = stated_by_lines(lines);
  const std::map<TargetHost, Stated> given = given_by_rows(rows);
  for (const auto& [key, s] : stated) {
    const auto g = given.find(key);
    std::string why = left_out(key, s, g == given.end() ? Stated{} : g->second);
    if (!why.empty()) {
      return std::unexpected(std::move(why));
    }
  }
  for (const VerdictRow& row : rows) {
    const auto s = stated.find({row.target, row.host});
    if (s == stated.end()) {
      return std::unexpected("results/verdicts.csv: " + target_on({row.target, row.host}) +
                             " has no \"Measured\" line in BENCHMARKS.md that names that host");
    }
    if (!states(s->second, row)) {
      return std::unexpected("results/verdicts.csv: " + target_on({row.target, row.host}) + " is " +
                             row.verdict + ", which no \"Measured\" line of BENCHMARKS.md for it " +
                             "states");
    }
  }
  return {};
}

std::expected<void, std::string> check_cited_files(const std::vector<VerdictRow>& rows,
                                                   const Hosts& hosts) {
  for (const VerdictRow& row : rows) {
    for (const std::string& path : row.files) {
      const std::string name = path.substr(path.rfind('/') + 1);
      const auto host = hosts.find(row.host);
      const bool present = host != hosts.end() &&
                           std::ranges::find(host->second.results, name, &ResultsEntry::name) !=
                               host->second.results.end();
      if (!present) {
        return std::unexpected("results/verdicts.csv: " + path +
                               " is not a results file in results/" + row.host + "/");
      }
    }
  }
  return {};
}

std::string headline_key(const HeadlineRow& row) {
  return row.detail.empty() ? row.source : row.source + " " + row.detail;
}

// met, missed, or none for an INVALID run.
std::string_view headline_verdict(const Evaluated& cell) {
  if (cell.verdict.starts_with("met")) {
    return "met";
  }
  return cell.verdict.starts_with("missed") ? "missed" : "none";
}

// Every headline row but an oracle run is named by exactly one verdict row, which agrees with
// it; a verdict row names only a headline row that exists.
std::expected<void, std::string> check_against_headline(const std::vector<VerdictRow>& rows,
                                                        const std::vector<HeadlineRow>& spec,
                                                        const std::vector<Evaluated>& cells) {
  for (std::size_t i = 0; i < spec.size(); ++i) {
    if (spec[i].kind == kOracleRunKind) {
      continue;
    }
    const std::string key = headline_key(spec[i]);
    const std::string what = "headline " + spec[i].id + " on " + spec[i].host + " (" + key + ")";
    std::size_t count = 0;
    for (const VerdictRow& row : rows) {
      if (row.target != spec[i].id || row.host != spec[i].host || row.headline != key) {
        continue;
      }
      ++count;
      if (row.verdict != headline_verdict(cells[i])) {
        return std::unexpected(what + " computes " + std::string(headline_verdict(cells[i])) +
                               ", and results/verdicts.csv says " + row.verdict);
      }
    }
    if (count != 1) {
      return std::unexpected(what + " is named by " + std::to_string(count) +
                             " rows of results/verdicts.csv, not one");
    }
  }
  for (const VerdictRow& row : rows) {
    const bool named = std::ranges::any_of(spec, [&row](const HeadlineRow& h) {
      return h.kind != kOracleRunKind && h.id == row.target && h.host == row.host &&
             headline_key(h) == row.headline;
    });
    if (!row.headline.empty() && !named) {
      return std::unexpected("results/verdicts.csv: " + target_on({row.target, row.host}) +
                             " names headline row " + row.headline +
                             ", which results/headline.csv does not have");
    }
  }
  return {};
}

struct TargetLink {
  std::size_t line = 0;  // the target's first "Measured" line, for the order
  std::string anchor;
};

std::expected<std::map<std::string, TargetLink, std::less<>>, std::string> target_links(
    const std::vector<MeasuredLine>& lines) {
  std::map<std::string, TargetLink, std::less<>> links;
  for (const MeasuredLine& m : lines) {
    if (m.anchor.empty()) {
      return std::unexpected("BENCHMARKS.md: the heading above line " + std::to_string(m.line) +
                             " has a byte outside ASCII, so its anchor is not known");
    }
    links.emplace(m.target, TargetLink{.line = m.line, .anchor = m.anchor});
  }
  return links;
}

std::string target_link(std::string_view target,
                        const std::map<std::string, TargetLink, std::less<>>& links) {
  return "[" + std::string(target) + "](BENCHMARKS.md#" + links.find(target)->second.anchor + ")";
}

// "met at 1.05": a run's outcome and figure as words.
std::string run_words(std::string_view run) {
  const std::size_t space = run.find(' ');
  return std::string(run.substr(0, space)) + " at " + std::string(run.substr(space + 1));
}

// Why a row is not counted, with both runs where it has them: "one thread: the withdrawn run met
// at 1.05, the rerun missed at 1.12".
std::string uncounted_runs(const VerdictRow& row) {
  std::string out = row.condition.empty() ? std::string() : row.condition + ": ";
  if (row.verdict == "split") {
    return out + "the withdrawn run " + run_words(row.withdrawn) + ", the rerun " +
           run_words(row.rerun);
  }
  out += "marked, failed the rule on every run, its last run " + run_words(row.rerun);
  return out + (row.withdrawn.empty() ? "" : ", the withdrawn run " + run_words(row.withdrawn));
}

// One host's rows, gathered for its line of the counts.
struct HostTally {
  std::map<std::size_t, std::string> order;  // first line -> target, counted rows only
  std::map<std::string, Stated, std::less<>> by_target;
  std::vector<std::string> commits;      // of the cited files, in backticks, first seen first
  std::vector<std::string> not_counted;  // each linked, with its runs
  std::vector<std::string> waiting;      // each linked, with what waits
  std::map<std::string, std::string, std::less<>> none_conditions;  // what a none row says
  bool tests = false;
};

void add_commits(const VerdictRow& row, const HostFiles& files, std::vector<std::string>& out) {
  for (const std::string& path : row.files) {
    const auto entry =
        std::ranges::find(files.results, path.substr(path.rfind('/') + 1), &ResultsEntry::name);
    const std::string commit = "`" + short_commit(entry->file.required("commit")) + "`";
    if (std::ranges::find(out, commit) == out.end()) {
      out.push_back(commit);
    }
  }
}

HostTally tally_host(std::string_view host, const std::vector<VerdictRow>& rows,
                     const std::map<std::string, TargetLink, std::less<>>& links,
                     const Hosts& hosts) {
  HostTally t;
  for (const VerdictRow& row : rows) {
    if (row.host != host) {
      continue;
    }
    if (is_waiting(row)) {
      t.waiting.push_back(target_link(row.target, links) +
                          (row.condition.empty() ? "" : " (" + row.condition + ")"));
      continue;
    }
    if (!is_counted(row)) {
      t.not_counted.push_back(target_link(row.target, links) + " (" + uncounted_runs(row) + ")");
    } else {
      t.order.emplace(links.find(row.target)->second.line, row.target);
    }
    Stated& s = t.by_target[row.target];
    s.met = s.met || row.verdict == "met";
    s.missed = s.missed || row.verdict == "missed";
    s.none = s.none || row.verdict == "none";
    if (row.verdict == "none" && !row.condition.empty()) {
      std::string& said = t.none_conditions[row.target];
      said += (said.empty() ? "" : "; ") + row.condition;
    }
    t.tests = t.tests || row.basis == "test";
    if (!row.files.empty()) {
      add_commits(row, hosts.find(host)->second, t.commits);
    }
  }
  return t;
}

// "`linux-zen1`, from the files of commit `8bb2116` and from tests".
std::string host_heading(std::string_view host, const HostTally& t) {
  if (host == kAnyHost) {
    return "By tests whose \"Measured\" line names no host";
  }
  std::string out = "`" + std::string(host) + "`, from ";
  if (!t.commits.empty()) {
    out += t.commits.size() == 1 ? "the files of commit " : "the files of commits ";
    out += joined(t.commits);
    out += t.tests ? " and from " : "";
  }
  return out + (t.tests ? "tests" : "");
}

// " Met, 2: a and b." or " Met, 0."
std::string group(std::string_view word, const std::vector<std::string>& items) {
  const std::string out = " " + std::string(word) + ", " + std::to_string(items.size());
  return out + (items.empty() ? "." : ": " + joined(items) + ".");
}

// One host's line of the counts: its targets judged, met, missed and met in part, each named and
// linked, then its rows not counted with both runs, then those measured with no verdict.
std::string host_counts(std::string_view host, const std::vector<VerdictRow>& rows,
                        const std::map<std::string, TargetLink, std::less<>>& links,
                        const Hosts& hosts) {
  const HostTally t = tally_host(host, rows, links, hosts);
  std::vector<std::string> met;
  std::vector<std::string> missed;
  std::vector<std::string> part;
  std::vector<std::string> none;
  for (const auto& [line, target] : t.order) {
    const Stated& s = t.by_target.at(target);
    const std::string linked = target_link(target, links);
    if (s.met && s.missed) {
      part.push_back(linked);
    } else if (s.met) {
      met.push_back(linked);
    } else if (s.missed) {
      missed.push_back(linked);
    } else {
      const auto said = t.none_conditions.find(target);
      none.push_back(linked + (said == t.none_conditions.end() ? "" : " (" + said->second + ")"));
    }
  }
  const std::size_t judged = met.size() + missed.size() + part.size();
  std::string out = "- " + host_heading(host, t) + ": " + std::to_string(judged) +
                    (judged == 1 ? " target" : " targets") + " judged.";
  out += group("Met", met);
  out += group("Missed", missed);
  out += group("Met in part", part);
  out += group("Not counted", t.not_counted);
  if (!none.empty()) {
    out += " Measured with no verdict stated: " + joined(none) + ".";
  }
  if (!t.waiting.empty()) {
    out += " Waiting for a run, neither judged nor counted: " + joined(t.waiting) + ".";
  }
  return out + "\n";
}

std::string verdict_counts(const std::vector<VerdictRow>& rows,
                           const std::map<std::string, TargetLink, std::less<>>& links,
                           const Hosts& hosts, bool in_results_md) {
  std::string out =
      "Every verdict: [`results/verdicts.csv`](results/verdicts.csv) has one row for each verdict "
      "that a \"Measured\" line of [BENCHMARKS.md](BENCHMARKS.md) states, by target, host and "
      "condition, and " +
      std::string(in_results_md ? "the table below" : "[RESULTS.md](RESULTS.md#verdicts)") +
      " lists every row with its files and notes. The generator refuses a target, host and "
      "verdict that a line states and no row gives, a row that no line states, a row that "
      "disagrees with the headline row it names, and a cited file that is not in "
      "`results/<host>/`. A target counts as met on a host if every one of its counted rows "
      "there is met, as missed if every one is missed, and as met in part if some are met and "
      "some missed, so a target with a missed row is never counted as met. A row is not "
      "counted, neither as met nor as missed, where a step withdrawn under the host's rule gave "
      "one outcome in the withdrawn run and the other in its rerun (the row gives both figures "
      "and both outcomes), or where its figure rests on a step that failed the rule on every run "
      "it was allowed (published with its mark); such a row is listed as not counted and names "
      "no headline row. A target whose step has no standing run on a host yet, withdrawn and to "
      "run again, has a waiting row there, neither judged nor counted, listed after the counts "
      "with what waits.\n\n";
  std::vector<std::string_view> order(kHostLabels.begin(), kHostLabels.end());
  order.push_back(kAnyHost);
  for (const std::string_view host : order) {
    if (std::ranges::any_of(rows, [host](const VerdictRow& row) { return row.host == host; })) {
      out += host_counts(host, rows, links, hosts);
    }
  }
  return out;
}

// A verdicts row's verdict cell.
std::string verdict_cell(const VerdictRow& row) {
  if (row.verdict == "split") {
    return "not counted: the runs differ";
  }
  if (row.verdict == "marked") {
    return "not counted: marked";
  }
  if (is_waiting(row)) {
    return "waiting for a run";
  }
  return row.verdict == "none" ? "none stated" : row.verdict;
}

// A verdicts row's runs cell: "withdrawn run met at 1.05; rerun missed at 1.12", or "-".
std::string runs_cell(const VerdictRow& row) {
  std::string runs = row.withdrawn.empty() ? "" : "withdrawn run " + run_words(row.withdrawn);
  if (!row.rerun.empty()) {
    runs += runs.empty() ? "" : "; ";
    runs += row.verdict == "marked" ? "last run " : "rerun ";
    runs += run_words(row.rerun);
  }
  return runs.empty() ? "-" : cell(runs);
}

// A verdicts row's files cell: each file linked, or "a test".
std::string files_cell(const VerdictRow& row) {
  if (row.basis == "test") {
    return "a test";
  }
  if (row.basis == "pending") {
    return "none yet";
  }
  std::string files;
  for (const std::string& path : row.files) {
    files += files.empty() ? "" : ", ";
    files += file_link(row.host, path.substr(path.rfind('/') + 1));
  }
  return files;
}

std::string verdicts_table(const std::vector<VerdictRow>& rows,
                           const std::map<std::string, TargetLink, std::less<>>& links) {
  std::string out =
      "| target | host | condition | verdict | runs | headline row | files | note |\n"
      "|---|---|---|---|---|---|---|---|\n";
  for (const VerdictRow& row : rows) {
    out += "| " + target_link(row.target, links) + " | " + row.host + " | " +
           (row.condition.empty() ? "-" : cell(row.condition)) + " | " + verdict_cell(row) + " | " +
           runs_cell(row) + " | " + (row.headline.empty() ? "-" : "`" + row.headline + "`") +
           " | " + files_cell(row) + " | " + (row.note.empty() ? "-" : cell(row.note)) + " |\n";
  }
  return out;
}

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
  const auto verdicts = parse_verdicts(inputs.verdicts);
  if (!verdicts) {
    return std::unexpected(verdicts.error());
  }
  const std::vector<MeasuredLine> lines = measured_lines(inputs.benchmarks);
  const auto links = target_links(lines);
  if (!links) {
    return std::unexpected(links.error());
  }
  for (const auto& checked :
       {check_against_benchmarks(*verdicts, lines), check_cited_files(*verdicts, hosts),
        check_against_headline(*verdicts, spec->rows, cells)}) {
    if (!checked) {
      return std::unexpected(checked.error());
    }
  }
  const std::string note = hosts_note(spec->rows, cells, hosts);
  Docs docs{.headline = headline_table(cells, false) + "\n" + note + "\n" +
                        verdict_counts(*verdicts, *links, hosts, false),
            .results_md = std::string(kResultsIntro) + headline_table(cells, true) + "\n" + note +
                          "\n## Verdicts\n\n" + verdict_counts(*verdicts, *links, hosts, true) +
                          "\n" + verdicts_table(*verdicts, *links) + "\n" +
                          measured_runs_section(*spec, runs, hosts)};
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
