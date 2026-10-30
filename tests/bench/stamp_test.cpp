#include "results/stamp.hpp"

#include <gtest/gtest.h>

#include <span>
#include <string>

#include "fixtures.hpp"
#include "results/json_lite.hpp"
#include "results/measure_line.hpp"

namespace aadp::results {
namespace {

using testing::StampCase;

void expect_refused(StampCase& test_case, StampError error) {
  const auto out = test_case.run();
  ASSERT_FALSE(out.has_value()) << "stamped a run that should have been refused";
  EXPECT_EQ(out.error().error, error) << out.error().detail;
}

TEST(Stamp, AGoodRunIsStampedReportable) {
  StampCase test_case;
  const auto out = test_case.run();
  ASSERT_TRUE(out.has_value()) << out.error().detail;
  const auto file = parse_results_file(*out);
  ASSERT_TRUE(file.has_value()) << file.error();
  EXPECT_EQ(file->value("reportable"), "yes");
  EXPECT_EQ(file->value("reportable_reason"), "none");
  EXPECT_EQ(file->value("claim"), "none");
  EXPECT_TRUE(missing_stamped_keys(*file).empty());
  EXPECT_TRUE(validate_stamped(*file).has_value());
}

TEST(Stamp, DropsThePrivateFieldsAndKeepsLoadTimeExitGovernorFrequency) {
  StampCase test_case;
  const auto out = test_case.run();
  ASSERT_TRUE(out.has_value()) << out.error().detail;
  EXPECT_EQ(out->find("host="), std::string::npos);
  EXPECT_EQ(out->find("host_name"), std::string::npos);
  EXPECT_EQ(out->find("executable"), std::string::npos);
  EXPECT_EQ(out->find(testing::kHost), std::string::npos);
  EXPECT_EQ(out->find("alice"), std::string::npos);

  EXPECT_NE(out->find("load1=0.31"), std::string::npos);
  EXPECT_NE(out->find("load1=0.42"), std::string::npos);
  EXPECT_NE(out->find("at=2026-01-02T03:04:05Z"), std::string::npos);
  EXPECT_NE(out->find("exit=0"), std::string::npos);
  EXPECT_NE(out->find("governor=performance"), std::string::npos);
  EXPECT_NE(out->find("freq_khz=cpu4:3400000"), std::string::npos);
  EXPECT_NE(out->find("\"load_avg\""), std::string::npos);
  EXPECT_NE(out->find("\"date\""), std::string::npos);
}

TEST(Stamp, RefusesNoStartLine) {
  StampCase test_case;
  test_case.captured = testing::end_line() + "\n";
  expect_refused(test_case, StampError::kNoStartLine);
}

TEST(Stamp, RefusesNoEndLine) {
  StampCase test_case;
  test_case.captured = testing::start_line() + "\n";
  expect_refused(test_case, StampError::kNoEndLine);
}

TEST(Stamp, RefusesAnEmptyCapture) {
  StampCase test_case;
  test_case.captured.clear();
  expect_refused(test_case, StampError::kNoStartLine);
}

TEST(Stamp, RefusesNonZeroExit) {
  StampCase test_case;
  test_case.captured = testing::start_line() + "\n" + testing::end_line(1) + "\n";
  expect_refused(test_case, StampError::kNonZeroExit);
}

TEST(Stamp, RefusesUnparsableLoad) {
  StampCase test_case;
  test_case.captured = "measure.sh: start load1=high host=testbox at=2026-01-02T03:04:05Z\n" +
                       testing::end_line() + "\n";
  expect_refused(test_case, StampError::kBadMeasureLine);
}

TEST(Stamp, RefusesTwoRunsInOneCapture) {
  StampCase test_case;
  test_case.captured = testing::captured_stderr() + testing::captured_stderr();
  expect_refused(test_case, StampError::kRepeatedLine);
}

TEST(Stamp, RefusesAFileFromAnotherHost) {
  StampCase test_case;
  test_case.request.current_host_label = "mac-m4max";
  expect_refused(test_case, StampError::kWrongHost);
}

TEST(Stamp, RefusesAMeasureLineFromAnotherMachine) {
  StampCase test_case;
  test_case.captured = testing::start_line("elsewhere") + "\n" + testing::end_line() + "\n";
  expect_refused(test_case, StampError::kWrongHost);
}

TEST(Stamp, ComparesHostnamesInShortFormWithoutCase) {
  StampCase test_case;
  test_case.request.current_hostname = "TestBox.local";
  const auto out = test_case.run();
  ASSERT_TRUE(out.has_value()) << out.error().detail;
}

TEST(Stamp, RefusesHostOtherEvenWhenItMatches) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "host_label", "other");
  test_case.raw = testing::text_of(raw);
  test_case.request.current_host_label = "other";
  expect_refused(test_case, StampError::kOtherHost);
}

TEST(Stamp, RefusesADirtyTree) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "tree_state", "dirty");
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kDirtyTree);
}

TEST(Stamp, RefusesAnUnknownTree) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "tree_state", "unknown");
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kDirtyTree);
}

TEST(Stamp, RefusesAMissingCommit) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "commit", "unknown");
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kNoCommit);
}

TEST(Stamp, RefusesADebugBuild) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "build_type", "Debug");
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kNotRelease);
}

TEST(Stamp, RefusesAnUnpinnedZen1Run) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "pinning_state", "not pinned");
  testing::set_field(raw, "pinned_cpus", "none");
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kNotPinned);
}

TEST(Stamp, RefusesAPinOutsideWhatMeasureReportedOn) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "pinned_cpus", "5");
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kPinMismatch);
}

TEST(Stamp, AcceptsAnUnpinnedMacRun) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "host_label", "mac-m4max");
  testing::set_field(raw, "pinning_state", "not pinned");
  testing::set_field(raw, "pinned_cpus", "none");
  test_case.raw = testing::text_of(raw);
  test_case.captured =
      "measure.sh: start load1=1.0 host=testbox at=2026-01-02T03:04:05Z\n"
      "measure.sh: end load1=1.1 exit=0 at=2026-01-02T03:04:09Z\n";
  test_case.request.current_host_label = "mac-m4max";
  const auto out = test_case.run();
  ASSERT_TRUE(out.has_value()) << out.error().detail;
}

TEST(Stamp, RefusesACaptureFromAnotherRun) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "run_started_at", "2026-01-02T03:05:00Z");
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kStaleCapture);
}

TEST(Stamp, RefusesEachMissingHeaderKey) {
  for (const auto keys : {std::span<const std::string_view>(kRunHeaderKeys),
                          std::span<const std::string_view>(kPerfHeaderKeys)}) {
    for (const std::string_view key : keys) {
      StampCase test_case;
      ResultsFile raw = testing::raw_file();
      testing::remove_field(raw, key);
      test_case.raw = testing::text_of(raw);
      expect_refused(test_case, StampError::kIncompleteHeader);
    }
  }
}

TEST(Stamp, RefusesRawOutputThatAlreadyClaimsToBeStamped) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  raw.header.push_back({"reportable", "yes"});
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kMalformedRun);
}

TEST(Stamp, RefusesBrokenBenchmarkJson) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  raw.sections.front().body = "{ not json\n";
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kMalformedRun);
}

TEST(Stamp, RefusesTheHostnameLeftInTheOutput) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "cpu_model", "built on TESTBOX");
  test_case.raw = testing::text_of(raw);
  const auto out = test_case.run();
  ASSERT_FALSE(out.has_value());
  EXPECT_EQ(out.error().error, StampError::kPrivateData);
  EXPECT_NE(out.error().detail.find("hostname"), std::string::npos);
  EXPECT_EQ(out.error().detail.find("TESTBOX"), std::string::npos) << "refusal repeats the secret";
}

TEST(Stamp, TakesTheHostnameFromTheStartLineToo) {
  // The tool's own name for the machine differs from the one measure.sh printed.
  StampCase test_case;
  test_case.request.reportable = false;
  test_case.request.not_reportable_reason = "test";
  test_case.request.current_hostname = "somewhere";
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "kernel", "testbox");
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kPrivateData);
}

TEST(Stamp, RefusesAHomePathLeftInTheOutput) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "cxx_flags", "-fdebug-prefix-map=" + testing::home_path("src") + "=.");
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kPrivateData);
}

// Found by fuzz_results_file: the scan knew only the hostnames it was given.
TEST(Stamp, RefusesAHostFieldNamingAnotherMachine) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  raw.sections.back().body += "copied: measure.sh: start load1=1 host=elsewhere\n";
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kPrivateData);
}

// Also found by fuzz_results_file: only the benchmark JSON section is filtered.
TEST(Stamp, RefusesAGoogleBenchmarkKeyOutsideTheBenchmarkJson) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  raw.sections.back().body += "\"executable\": \"./bench\"\n";
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kPrivateData);
}

TEST(Stamp, RefusesAnAddressLeftInTheOutput) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  raw.sections.back().body += "note from 203.0.113.7\n";
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kPrivateData);
}

TEST(Stamp, ScansBeforeWritingEvenWhenNotReportable) {
  StampCase test_case;
  test_case.request.reportable = false;
  test_case.request.not_reportable_reason = "from " + testing::home_path("x");
  expect_refused(test_case, StampError::kPrivateData);
}

TEST(Stamp, NotReportableNeedsAReason) {
  StampCase test_case;
  test_case.request.reportable = false;
  expect_refused(test_case, StampError::kNoReason);
}

TEST(Stamp, NotReportableRunWithoutMeasureLinesSaysSo) {
  StampCase test_case;
  test_case.request.reportable = false;
  test_case.request.not_reportable_reason = "measure.sh not found";
  test_case.captured.clear();
  const auto out = test_case.run();
  ASSERT_TRUE(out.has_value()) << out.error().detail;
  const auto file = parse_results_file(*out);
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(file->value("reportable"), "no");
  EXPECT_EQ(file->value("reportable_reason"), "measure.sh not found");
  EXPECT_EQ(file->value("measure_start"), "absent");
  EXPECT_EQ(file->value("measure_end"), "absent");
}

TEST(Validate, RefusesAFileEditedIntoReportable) {
  StampCase test_case;
  test_case.request.reportable = false;
  test_case.request.not_reportable_reason = "test";
  test_case.captured.clear();
  auto file = parse_results_file(*test_case.run());
  ASSERT_TRUE(file.has_value());
  EXPECT_TRUE(validate_stamped(*file).has_value());
  testing::set_field(*file, "reportable", "yes");
  EXPECT_FALSE(validate_stamped(*file).has_value());
}

}  // namespace
}  // namespace aadp::results
