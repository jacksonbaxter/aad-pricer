// Combines a binary's raw output with the stderr captured around it, into a results file.
// Everything here is a refusal rule, so the tool around it is only file handling.
//
// Dropped: `host=`, and Google Benchmark's host_name and executable.
// Kept: load, time, exit, governor and frequency, from validated fields.
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "results/privacy.hpp"

namespace aadp::results {

enum class StampError {
  kMalformedRun,      // The binary's output cannot be read
  kIncompleteHeader,  // A required key is missing or empty
  kNoStartLine,
  kNoEndLine,
  kRepeatedLine,    // Two starts or two ends: a capture from more than one run
  kBadMeasureLine,  // A start or end line whose fields do not parse
  kNonZeroExit,
  kWrongHost,
  kOtherHost,  // `other` is never reportable
  kDirtyTree,
  kNoCommit,
  kNotRelease,
  kNotPinned,     // linux-zen1 numbers are pinned or they are not reported
  kPinMismatch,   // The pinned CPU is not the one measure.sh reported on
  kStaleCapture,  // The binary did not run between the two lines
  kNoReason,      // Not reportable, and not saying why
  kPrivateData,
};

struct StampRefusal {
  StampError error;
  std::string detail;
};

struct StampRequest {
  std::string_view raw_output;
  std::string_view captured_stderr;
  bool reportable = false;
  std::string not_reportable_reason;  // Required when reportable is false
  std::string current_host_label;
  std::string current_hostname;
  PrivacyConfig privacy;
};

std::expected<std::string, StampRefusal> stamp_results(const StampRequest& request);

}  // namespace aadp::results
