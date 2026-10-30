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
  return std::unexpected(StampRefusal{error, std::move(detail)});
}

std::string short_lower(std::string_view name) {
  std::string out(name.substr(0, name.find('.')));
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// The checks that only a reportable run has to pass. The start and end lines are returned to
// the caller through the scan.
std::expected<void, StampRefusal> check_reportable(const ResultsFile& run, const MeasureScan& scan,
                                                   const StampRequest& request) {
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
  const MeasureLine& start = scan.starts.front();
  const MeasureLine& end = scan.ends.front();
  if (end.exit_status != 0) {
    return refuse(StampError::kNonZeroExit,
                  "end line has exit=" + std::to_string(*end.exit_status));
  }
  if (*run.value("host_label") == "other") {
    return refuse(StampError::kOtherHost, "host other is never reportable");
  }
  if (*run.value("host_label") != request.current_host_label) {
    return refuse(StampError::kWrongHost, "file is from " + std::string(*run.value("host_label")) +
                                              ", stamping on " + request.current_host_label);
  }
  if (!start.host) {
    return refuse(StampError::kWrongHost, "start line has no host");
  }
  // Short forms, because hostname -s and gethostname differ on macOS. Neither value goes into
  // the message.
  if (short_lower(*start.host) != short_lower(request.current_hostname)) {
    return refuse(StampError::kWrongHost, "start line names another machine");
  }
  const std::string_view tree = *run.value("tree_state");
  if (tree != "clean" && tree != "exported") {
    return refuse(StampError::kDirtyTree, "working tree is " + std::string(tree));
  }
  if (!is_commit_hash(*run.value("commit"))) {
    return refuse(StampError::kNoCommit, "no commit recorded");
  }
  if (*run.value("build_type") != "Release") {
    return refuse(StampError::kNotRelease,
                  "build type is " + std::string(*run.value("build_type")));
  }
  const std::string_view at = *run.value("run_started_at");
  if (at < std::string_view(start.at) || at > std::string_view(end.at)) {
    return refuse(StampError::kStaleCapture, "the binary did not run between the two lines");
  }

  const std::string_view pinning = *run.value("pinning_state");
  if (*run.value("host_label") == "linux-zen1" && pinning != "pinned") {
    return refuse(StampError::kNotPinned, "linux-zen1 run is not pinned");
  }
  if (pinning == "pinned" && start.cpus) {
    const auto cpus = expand_cpu_list(*start.cpus);
    if (!cpus) {
      return refuse(StampError::kBadMeasureLine, cpus.error());
    }
    const auto pinned = expand_cpu_list(*run.value("pinned_cpus"));
    if (!pinned || pinned->size() != 1 ||
        std::ranges::find(*cpus, pinned->front()) == cpus->end()) {
      return refuse(StampError::kPinMismatch,
                    "pinned CPU " + std::string(*run.value("pinned_cpus")) +
                        " is not in the measure.sh CPU list " + *start.cpus);
    }
  }
  return {};
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

std::expected<std::string, StampRefusal> stamp_results(const StampRequest& request) {
  auto run = parse_results_file(request.raw_output);
  if (!run) {
    return refuse(StampError::kMalformedRun, run.error());
  }
  for (const std::string_view key : kStampHeaderKeys) {
    if (run->value(key)) {
      return refuse(StampError::kMalformedRun, "raw output already has " + std::string(key));
    }
  }
  for (const auto keys : {std::span<const std::string_view>(kRunHeaderKeys),
                          std::span<const std::string_view>(kPerfHeaderKeys)}) {
    const auto missing = missing_keys(*run, keys);
    if (!missing.empty()) {
      return refuse(StampError::kIncompleteHeader,
                    "header is missing " + std::string(missing.front()));
    }
  }
  if (run->section(kBenchmarkSection) == nullptr) {
    return refuse(StampError::kMalformedRun, "no benchmark section");
  }

  const MeasureScan scan = scan_measure_stderr(request.captured_stderr);
  if (request.reportable) {
    if (auto checked = check_reportable(*run, scan, request); !checked) {
      return std::unexpected(checked.error());
    }
  } else if (request.not_reportable_reason.empty()) {
    return refuse(StampError::kNoReason, "not reportable, and no reason given");
  }

  ResultsFile out;
  out.header.push_back({"claim", std::string(*run->value("claim"))});
  out.header.push_back({"reportable", request.reportable ? "yes" : "no"});
  out.header.push_back(
      {"reportable_reason", request.reportable ? "none" : request.not_reportable_reason});
  for (const HeaderField& field : run->header) {
    if (field.key != "claim") {
      out.header.push_back(field);
    }
  }
  out.header.push_back({"measure_start", scan.starts.empty()
                                             ? std::string(kAbsent)
                                             : sanitized_measure_line(scan.starts.front())});
  out.header.push_back({"measure_end", scan.ends.empty()
                                           ? std::string(kAbsent)
                                           : sanitized_measure_line(scan.ends.back())});

  for (const ResultsSection& section : run->sections) {
    if (section.name != kBenchmarkSection) {
      out.sections.push_back(section);
      continue;
    }
    std::string error;
    auto json = clean_benchmark_json(section.body, error);
    if (!json) {
      return refuse(StampError::kMalformedRun, "benchmark JSON: " + error);
    }
    out.sections.push_back({section.name, std::move(*json)});
  }

  if (request.reportable) {
    if (auto valid = validate_stamped(out); !valid) {
      return refuse(StampError::kMalformedRun, valid.error());
    }
  }
  auto text = write_results_file(out);
  if (!text) {
    return refuse(StampError::kMalformedRun, text.error());
  }

  PrivacyConfig privacy = request.privacy;
  privacy.hostnames.push_back(request.current_hostname);
  for (const MeasureLine& start : scan.starts) {
    if (start.host) {
      privacy.hostnames.push_back(*start.host);
    }
  }
  const auto hits = scan_private_text(*text, privacy);
  if (!hits.empty()) {
    std::string detail = "refusing to write:";
    for (const PrivacyHit& hit : hits) {
      detail += " line " + std::to_string(hit.line) + " has a " + std::string(hit.kind) + ";";
    }
    return refuse(StampError::kPrivateData, std::move(detail));
  }
  return std::move(*text);
}

}  // namespace aadp::results
