#include "results/stamp.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>

#include "results/json_lite.hpp"
#include "results/measure_line.hpp"
#include "results/results_file.hpp"

namespace aadp::results {
namespace {

constexpr std::array<std::string_view, 2> kDroppedJsonKeys = {"host_name", "executable"};
constexpr std::string_view kAbsent = "absent";

std::unexpected<StampRefusal> refuse(StampError error, std::string detail) {
  return std::unexpected(StampRefusal{.error = error, .detail = std::move(detail)});
}

std::string short_lower(std::string_view name) {
  std::string out(name.substr(0, name.find('.')));
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::expected<void, StampRefusal> check_measure_lines(const MeasureScan& scan) {
  if (!scan.malformed.empty()) {
    return refuse(StampError::kBadMeasureLine, scan.malformed.front());
  }
  if (scan.starts.empty()) {
    return refuse(StampError::kNoStartLine, "no measure.sh start line in the capture");
  }
  if (scan.ends.empty()) {
    return refuse(StampError::kNoEndLine, "no measure.sh end line in the capture");
  }
  if (scan.starts.size() > 1 || scan.ends.size() > 1) {
    return refuse(StampError::kRepeatedLine, "more than one start or end line in the capture");
  }
  return {};
}

std::expected<void, StampRefusal> check_host(const ResultsFile& run, const MeasureLine& start,
                                             const StampRequest& request) {
  const std::string_view label = run.required("host_label");
  if (label == "other") {
    return refuse(StampError::kOtherHost, "host other is never reportable");
  }
  if (label != request.current_host_label) {
    return refuse(StampError::kWrongHost, "file is from " + std::string(label) + ", stamping on " +
                                              request.current_host_label);
  }
  if (!start.host) {
    return refuse(StampError::kWrongHost, "start line has no host");
  }
  // Short forms, because hostname -s and gethostname differ on macOS. Neither value goes into
  // the message.
  if (short_lower(*start.host) != short_lower(request.current_hostname)) {
    return refuse(StampError::kWrongHost, "start line names another machine");
  }
  return {};
}

std::expected<void, StampRefusal> check_build(const ResultsFile& run, const MeasureLine& start,
                                              const MeasureLine& end) {
  const std::string_view tree = run.required("tree_state");
  if (tree != "clean" && tree != "exported") {
    return refuse(StampError::kDirtyTree, "working tree is " + std::string(tree));
  }
  if (!is_commit_hash(run.required("commit"))) {
    return refuse(StampError::kNoCommit, "no commit recorded");
  }
  if (run.required("build_type") != "Release") {
    return refuse(StampError::kNotRelease,
                  "build type is " + std::string(run.required("build_type")));
  }
  const std::string_view at = run.required("run_started_at");
  if (at < std::string_view(start.at) || at > std::string_view(end.at)) {
    return refuse(StampError::kStaleCapture, "the binary did not run between the two lines");
  }
  if (run.required("process_nice") != "0") {
    return refuse(StampError::kNiced, "ran at nice " + std::string(run.required("process_nice")));
  }
  return {};
}

std::expected<void, StampRefusal> check_pinning(const ResultsFile& run, const MeasureLine& start) {
  const std::string_view pinning = run.required("pinning_state");
  if (run.required("host_label") == "linux-zen1" && pinning != "pinned") {
    return refuse(StampError::kNotPinned, "linux-zen1 run is not pinned");
  }
  if (pinning != "pinned" || !start.cpus) {
    return {};
  }
  const auto cpus = expand_cpu_list(*start.cpus);
  if (!cpus) {
    return refuse(StampError::kBadMeasureLine, cpus.error());
  }
  const auto pinned = expand_cpu_list(run.required("pinned_cpus"));
  if (!pinned || pinned->size() != 1 || std::ranges::find(*cpus, pinned->front()) == cpus->end()) {
    return refuse(StampError::kPinMismatch,
                  "pinned CPU " + std::string(run.required("pinned_cpus")) +
                      " is not in the measure.sh CPU list " + *start.cpus);
  }
  return {};
}

// The checks that only a reportable run has to pass. The start and end lines are returned to
// the caller through the scan.
std::expected<void, StampRefusal> check_reportable(const ResultsFile& run, const MeasureScan& scan,
                                                   const StampRequest& request) {
  if (auto lines = check_measure_lines(scan); !lines) {
    return lines;
  }
  const MeasureLine& start = scan.starts.front();
  const MeasureLine& end = scan.ends.front();
  if (end.exit_status != 0) {
    return refuse(StampError::kNonZeroExit,
                  "end line has exit=" + std::to_string(end.exit_status.value_or(-1)));
  }
  if (auto host = check_host(run, start, request); !host) {
    return host;
  }
  if (auto build = check_build(run, start, end); !build) {
    return build;
  }
  return check_pinning(run, start);
}

std::optional<std::string> clean_benchmark_json(std::string_view body, std::string& error) {
  auto json = parse_json(body);
  if (!json) {
    error = json.error();
    return std::nullopt;
  }
  drop_json_keys(*json, kDroppedJsonKeys);
  return write_json(*json);
}

}  // namespace

namespace {

std::expected<void, StampRefusal> check_raw_run(const ResultsFile& run) {
  for (const std::string_view key : kStampHeaderKeys) {
    if (run.value(key)) {
      return refuse(StampError::kMalformedRun, "raw output already has " + std::string(key));
    }
  }
  for (const auto keys : {std::span<const std::string_view>(kRunHeaderKeys),
                          std::span<const std::string_view>(kPerfHeaderKeys)}) {
    const auto missing = missing_keys(run, keys);
    if (!missing.empty()) {
      return refuse(StampError::kIncompleteHeader,
                    "header is missing " + std::string(missing.front()));
    }
  }
  if (run.section(kBenchmarkSection) == nullptr) {
    return refuse(StampError::kMalformedRun, "no benchmark section");
  }
  return {};
}

std::expected<ResultsFile, StampRefusal> build_stamped(const ResultsFile& run,
                                                       const MeasureScan& scan,
                                                       const StampRequest& request) {
  ResultsFile out;
  out.header.push_back({.key = "claim", .value = std::string(run.required("claim"))});
  out.header.push_back({.key = "reportable", .value = request.reportable ? "yes" : "no"});
  out.header.push_back({.key = "reportable_reason",
                        .value = request.reportable ? "none" : request.not_reportable_reason});
  for (const HeaderField& field : run.header) {
    if (field.key != "claim") {
      out.header.push_back(field);
    }
  }
  out.header.push_back({.key = "measure_start",
                        .value = scan.starts.empty()
                                     ? std::string(kAbsent)
                                     : sanitized_measure_line(scan.starts.front())});
  out.header.push_back({.key = "measure_end",
                        .value = scan.ends.empty() ? std::string(kAbsent)
                                                   : sanitized_measure_line(scan.ends.back())});

  for (const ResultsSection& section : run.sections) {
    if (section.name != kBenchmarkSection) {
      out.sections.push_back(section);
      continue;
    }
    std::string error;
    auto json = clean_benchmark_json(section.body, error);
    if (!json) {
      return refuse(StampError::kMalformedRun, "benchmark JSON: " + error);
    }
    out.sections.push_back({.name = section.name, .body = std::move(*json)});
  }
  return out;
}

std::expected<void, StampRefusal> check_private_text(const std::string& text,
                                                     const MeasureScan& scan,
                                                     const StampRequest& request) {
  PrivacyConfig privacy = request.privacy;
  privacy.hostnames.push_back(request.current_hostname);
  for (const MeasureLine& start : scan.starts) {
    if (start.host) {
      privacy.hostnames.push_back(*start.host);
    }
  }
  const auto hits = scan_private_text(text, privacy);
  if (hits.empty()) {
    return {};
  }
  std::string detail = "refusing to write:";
  for (const PrivacyHit& hit : hits) {
    detail += " line " + std::to_string(hit.line) + " has a " + std::string(hit.kind) + ";";
  }
  return refuse(StampError::kPrivateData, std::move(detail));
}

}  // namespace

std::expected<std::string, StampRefusal> stamp_results(const StampRequest& request) {
  auto run = parse_results_file(request.raw_output);
  if (!run) {
    return refuse(StampError::kMalformedRun, run.error());
  }
  if (auto raw = check_raw_run(*run); !raw) {
    return std::unexpected(raw.error());
  }

  const MeasureScan scan = scan_measure_stderr(request.captured_stderr);
  if (request.reportable) {
    if (auto checked = check_reportable(*run, scan, request); !checked) {
      return std::unexpected(checked.error());
    }
  } else if (request.not_reportable_reason.empty()) {
    return refuse(StampError::kNoReason, "not reportable, and no reason given");
  }

  auto out = build_stamped(*run, scan, request);
  if (!out) {
    return std::unexpected(out.error());
  }
  if (request.reportable) {
    if (auto valid = validate_stamped(*out); !valid) {
      return refuse(StampError::kMalformedRun, valid.error());
    }
  }
  auto text = write_results_file(*out);
  if (!text) {
    return refuse(StampError::kMalformedRun, text.error());
  }
  if (auto clean = check_private_text(*text, scan, request); !clean) {
    return std::unexpected(clean.error());
  }
  return std::move(*text);
}

}  // namespace aadp::results
