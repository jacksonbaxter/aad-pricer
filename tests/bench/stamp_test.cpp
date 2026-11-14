#include "results/stamp.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "fixtures.hpp"
#include "results/report.hpp"
#include "results/results_file.hpp"

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

TEST(Stamp, RefusesARunAtAnotherPriority) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "process_nice", "5");
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kNiced);
}

TEST(Stamp, RefusesAPinOutsideWhatMeasureReportedOn) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  // Not cpu5: that is meant to be idle, which the stamp refuses for its own reason.
  testing::set_field(raw, "pinned_cpus", "7");
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kPinMismatch);
}

// A threaded row on linux-zen1: taskset -c 4,6 around the binary, so measure.sh reports on both.
std::string two_cpu_capture(std::string_view cpus) {
  return "measure.sh: start load1=0.31 host=testbox at=2026-01-02T03:04:05Z cpus=" +
         std::string(cpus) +
         " governor=performance freq_khz=cpu4:3500000,cpu6:3500000 perf_event_paranoid=2\n"
         "measure.sh: end load1=0.42 exit=0 at=2026-01-02T03:04:09Z cpus=" +
         std::string(cpus) +
         " governor=performance freq_khz=cpu4:3400000,cpu6:3400000 perf_event_paranoid=2\n";
}

TEST(Stamp, AcceptsATwoCpuPinThatMeasureReportedOn) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "pinned_cpus", "4,6");
  test_case.raw = testing::text_of(raw);
  test_case.captured = two_cpu_capture("4,6");
  const auto out = test_case.run();
  ASSERT_TRUE(out.has_value()) << out.error().detail;
  const auto file = parse_results_file(*out);
  ASSERT_TRUE(file.has_value()) << file.error();
  EXPECT_EQ(file->value("pinned_cpus"), "4,6");
  EXPECT_EQ(file->value(kMeantIdleCpusKey), "5 (not enforced)");
  EXPECT_NE(out->find("cpus=4,6 governor=performance"), std::string::npos);
}

// The first CPU matches, so only a check of every CPU of the mask sees the second one.
TEST(Stamp, RefusesASecondPinnedCpuOutsideWhatMeasureReportedOn) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "pinned_cpus", "4,6");
  test_case.raw = testing::text_of(raw);
  test_case.captured = two_cpu_capture("4");
  expect_refused(test_case, StampError::kPinMismatch);
}

TEST(Stamp, RefusesAPinToThreeCpus) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "pinned_cpus", "4,6-7");
  test_case.raw = testing::text_of(raw);
  test_case.captured = two_cpu_capture("4,6-7");
  expect_refused(test_case, StampError::kPinTooWide);
}

TEST(Stamp, RefusesAZen1RunThatNamesNoIdleCpu) {
  for (const std::string_view value : {"none", "5", "", "x (not enforced)"}) {
    StampCase test_case;
    ResultsFile raw = testing::raw_file();
    if (value.empty()) {
      testing::remove_field(raw, kMeantIdleCpusKey);
    } else {
      testing::set_field(raw, kMeantIdleCpusKey, value);
    }
    test_case.raw = testing::text_of(raw);
    SCOPED_TRACE(std::string(value));
    expect_refused(test_case, StampError::kNoIdleCpu);
  }
}

TEST(Stamp, RefusesAMeantIdleCpuInsideThePin) {
  StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "pinned_cpus", "4-5");
  test_case.raw = testing::text_of(raw);
  test_case.captured = two_cpu_capture("4-5");
  expect_refused(test_case, StampError::kIdleCpuPinned);
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
  raw.header.push_back({.key = "reportable", .value = "yes"});
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

// --- report files (results/report.hpp): every rule above, and the report rules ---

StampCase report_case(std::string_view body = testing::kReportBody) {
  StampCase test_case;
  test_case.raw = testing::text_of(testing::raw_report_file(body));
  return test_case;
}

std::string report_with(std::string_view from, std::string_view to) {
  std::string body(testing::kReportBody);
  body.replace(body.find(from), from.size(), to);
  return body;
}

TEST(StampReport, AGoodReportRunIsStampedReportableWithItsSectionsUnchanged) {
  StampCase test_case = report_case();
  const auto out = test_case.run();
  ASSERT_TRUE(out.has_value()) << out.error().detail;
  const auto file = parse_results_file(*out);
  ASSERT_TRUE(file.has_value()) << file.error();
  EXPECT_EQ(file->value("reportable"), "yes");
  EXPECT_TRUE(missing_stamped_keys(*file).empty());
  EXPECT_TRUE(validate_stamped(*file).has_value());
  ASSERT_EQ(file->sections.size(), 1U);
  EXPECT_EQ(file->sections.front().body, testing::kReportBody);
}

TEST(StampReport, AppliesTheRunRulesToAReportToo) {
  StampCase no_end = report_case();
  no_end.captured = testing::start_line() + "\n";
  expect_refused(no_end, StampError::kNoEndLine);
  StampCase dirty = report_case();
  ResultsFile raw = testing::raw_report_file();
  testing::set_field(raw, "tree_state", "dirty");
  dirty.raw = testing::text_of(raw);
  expect_refused(dirty, StampError::kDirtyTree);
  StampCase debug = report_case();
  testing::set_field(raw, "tree_state", "exported");
  testing::set_field(raw, "build_type", "Debug");
  debug.raw = testing::text_of(raw);
  expect_refused(debug, StampError::kNotRelease);
  StampCase missing = report_case();
  raw = testing::raw_report_file();
  testing::remove_field(raw, "perf_cycles");
  missing.raw = testing::text_of(raw);
  expect_refused(missing, StampError::kIncompleteHeader);
  StampCase home = report_case(report_with("title Fit error per slice", "title Fit error"));
  home.request.privacy.forbidden.emplace_back("Fit error");
  expect_refused(home, StampError::kPrivateData);
}

TEST(StampReport, RefusesAFileThatIsBothKindsOrNeither) {
  StampCase both;
  ResultsFile raw = testing::raw_file();
  raw.sections.push_back({.name = "report_fit_error", .body = std::string(testing::kReportBody)});
  both.raw = testing::text_of(raw);
  expect_refused(both, StampError::kFileKind);

  StampCase neither;
  raw = testing::raw_file();
  raw.sections.erase(raw.sections.begin());
  neither.raw = testing::text_of(raw);
  expect_refused(neither, StampError::kFileKind);
}

TEST(StampReport, RefusesEachMalformedTable) {
  for (const auto& [from, to] :
       std::initializer_list<std::pair<std::string_view, std::string_view>>{
           {"type table", "type chart"},
           {"timing no\n", ""},
           {"column quotes integer count", "column error integer count"},
           {"column class text none", "column class bad none"},
           {"row s2W4", "row s1"},
           {"row s2W4 1.5 51 no-arbitrage", "row s2W4 1.5 51"},
           {"row s2W4 1.5", "row s2W4 inf"},
           {"row s2W4 1.5", "row s2W4 nan"},
           {"row s2W4 1.5", "row s2W4 1e999"},
           {"row s2W4 1.5", "row s2W4 1.50"},
           {"no-arbitrage", "no|arbitrage"},
       }) {
    StampCase test_case = report_case(report_with(from, to));
    SCOPED_TRACE(std::string(from) + " -> " + std::string(to));
    expect_refused(test_case, StampError::kMalformedReport);
  }
}

TEST(StampReport, RefusesAnyOtherSectionInAReport) {
  StampCase test_case;
  ResultsFile raw = testing::raw_report_file();
  raw.sections.push_back({.name = "histogram_x", .body = "p50 1\n"});
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kMalformedReport);
}

TEST(StampReport, RefusesAnOversizedReport) {
  StampCase test_case;
  ResultsFile raw = testing::raw_report_file();
  raw.sections.push_back(
      {.name = "attribution", .body = std::string(kMaxReportFileBytes, 'x') + "\n"});
  test_case.raw = testing::text_of(raw);
  expect_refused(test_case, StampError::kMalformedReport);
}

TEST(StampReport, RefusesASectionFromTheChainWithoutTheAttribution) {
  const std::string chain = report_with("uses_chain no", "uses_chain yes");
  StampCase bare = report_case(chain);
  expect_refused(bare, StampError::kNoAttribution);

  StampCase partial;
  ResultsFile raw = testing::raw_report_file(chain);
  raw.sections.push_back({.name = "attribution", .body = "Taiwan Futures Exchange (TAIFEX)\n"});
  partial.raw = testing::text_of(raw);
  expect_refused(partial, StampError::kNoAttribution);

  StampCase whole;
  raw.sections.back().body = testing::attribution_body();
  whole.raw = testing::text_of(raw);
  const auto out = whole.run();
  ASSERT_TRUE(out.has_value()) << out.error().detail;
  EXPECT_TRUE(out->contains(testing::attribution_body()));
}

TEST(StampReport, AppliesTheReportRulesWhenNotReportableToo) {
  StampCase test_case = report_case(report_with("type table", "type chart"));
  test_case.request.reportable = false;
  test_case.request.not_reportable_reason = "iteration";
  expect_refused(test_case, StampError::kMalformedReport);
  StampCase chain = report_case(report_with("uses_chain no", "uses_chain yes"));
  chain.request.reportable = false;
  chain.request.not_reportable_reason = "iteration";
  expect_refused(chain, StampError::kNoAttribution);
}

TEST(Validate, RefusesAStampedReportEditedOutOfTheGrammar) {
  for (const bool reportable : {true, false}) {
    StampCase test_case = report_case();
    test_case.request.reportable = reportable;
    test_case.request.not_reportable_reason = reportable ? "" : "test";
    const auto out = test_case.run();
    ASSERT_TRUE(out.has_value()) << out.error().detail;
    auto file = parse_results_file(*out);
    ASSERT_TRUE(file.has_value());
    EXPECT_TRUE(validate_stamped(*file).has_value());
    ResultsFile repeated = *file;
    repeated.sections.front().body = report_with("row s2W4", "row s1");
    EXPECT_FALSE(validate_stamped(repeated).has_value());
    ResultsFile chain = *file;
    chain.sections.front().body = report_with("uses_chain no", "uses_chain yes");
    EXPECT_FALSE(validate_stamped(chain).has_value());
    ResultsFile both = *file;
    both.sections.push_back({.name = "benchmark_json", .body = "{}\n"});
    EXPECT_FALSE(validate_stamped(both).has_value());
  }
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
