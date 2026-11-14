#include "results/table.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "fixtures.hpp"
#include "results/report.hpp"
#include "results/results_file.hpp"

namespace aadp::results {
namespace {

ResultsFile stamped(std::string_view label = "linux-zen1") {
  testing::StampCase test_case;
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "host_label", label);
  test_case.raw = testing::text_of(raw);
  test_case.request.current_host_label = std::string(label);
  return *parse_results_file(*test_case.run());
}

TEST(Table, RendersOneRowPerBenchmark) {
  const std::vector<ResultsFile> files = {stamped()};
  const auto table = render_host_table(files);
  ASSERT_TRUE(table.has_value()) << table.error();
  EXPECT_NE(table->find("### linux-zen1"), std::string::npos);
  EXPECT_NE(table->find("| harness_selftest | 3 | 60000 | 4100.5 | 4100 | 4300 | 90000 | 0.31 | "
                        "0.42 | 0123456789ab | none |"),
            std::string::npos)
      << *table;
}

TEST(Table, SaysThatClaimNoneIsNotAResult) {
  const std::vector<ResultsFile> files = {stamped()};
  EXPECT_NE(render_host_table(files)->find("not performance results"), std::string::npos);
}

TEST(Table, RefusesTwoHostsInOneTable) {
  const std::vector<ResultsFile> files = {stamped("linux-zen1"), stamped("mac-m4max")};
  const auto table = render_host_table(files);
  ASSERT_FALSE(table.has_value());
  EXPECT_NE(table.error().find("refusing"), std::string::npos);
}

TEST(Table, SplitsHostsIntoTheirOwnTables) {
  const std::vector<ResultsFile> files = {stamped("mac-m4max"), stamped("linux-zen1"),
                                          stamped("mac-m4max")};
  const auto tables = render_tables(files);
  ASSERT_TRUE(tables.has_value()) << tables.error();
  const auto zen = tables->find("### linux-zen1");
  const auto mac = tables->find("### mac-m4max");
  ASSERT_NE(zen, std::string::npos);
  ASSERT_NE(mac, std::string::npos);
  EXPECT_LT(zen, mac);
  const auto zen_row = tables->find("| harness_selftest");
  EXPECT_LT(zen_row, mac);
  const auto mac_row = tables->find("| harness_selftest", mac);
  ASSERT_NE(mac_row, std::string::npos);
  EXPECT_NE(tables->find("| harness_selftest", mac_row + 1), std::string::npos);
}

TEST(Table, RefusesAFileThatIsNotReportable) {
  testing::StampCase test_case;
  test_case.request.reportable = false;
  test_case.request.not_reportable_reason = "iteration run";
  const std::vector<ResultsFile> files = {*parse_results_file(*test_case.run())};
  const auto table = render_host_table(files);
  ASSERT_FALSE(table.has_value());
  EXPECT_NE(table.error().find("iteration run"), std::string::npos);
}

TEST(Table, RefusesAFileWithoutAHistogram) {
  ResultsFile file = stamped();
  std::erase_if(file.sections, [](const ResultsSection& section) {
    return section.name.starts_with(kHistogramPrefix);
  });
  const std::vector<ResultsFile> files = {file};
  EXPECT_FALSE(render_host_table(files).has_value());
}

TEST(Table, RefusesAnEmptyInput) {
  EXPECT_FALSE(render_host_table({}).has_value());
  EXPECT_FALSE(render_tables({}).has_value());
  EXPECT_FALSE(render_report_tables({}).has_value());
}

// --- report files ---

ResultsFile stamped_report(bool uses_chain = false, bool reportable = true) {
  testing::StampCase test_case;
  std::string body(testing::kReportBody);
  if (uses_chain) {
    body.replace(body.find("uses_chain no"), 13, "uses_chain yes");
  }
  ResultsFile raw = testing::raw_report_file(body);
  testing::set_field(raw, "claim", "result");
  if (uses_chain) {
    raw.sections.insert(raw.sections.begin(),
                        {.name = "attribution", .body = testing::attribution_body()});
  }
  test_case.raw = testing::text_of(raw);
  test_case.request.reportable = reportable;
  test_case.request.not_reportable_reason = reportable ? "" : "iteration";
  const auto out = test_case.run();
  EXPECT_TRUE(out.has_value()) << out.error().detail;
  return *parse_results_file(out.value_or(""));
}

TEST(ReportTables, RenderEachSectionWithItsUnitsCommitAndClaim) {
  const std::vector<ResultsFile> files = {stamped_report()};
  const auto tables = render_report_tables(files);
  ASSERT_TRUE(tables.has_value()) << tables.error();
  EXPECT_EQ(*tables,
            "#### Fit error per slice\n\n"
            "`report_fit_error`, commit `0123456789ab`, claim `result`, started "
            "2026-01-02T03:04:06Z. Targets: SVI-6, SVI-11. Timings: no. Derived from the option "
            "chain: no.\n\n"
            "| row | error (bp_of_F) | quotes (count) | class |\n"
            "|---|---|---|---|\n"
            "| s1 | 0.25 | 40 | type-1 |\n"
            "| s2W4 | 1.5 | 51 | no-arbitrage |\n");
}

TEST(ReportTables, CarryTheAttributionOnceWhenASectionUsesTheChain) {
  const std::vector<ResultsFile> files = {stamped_report(true), stamped_report(true)};
  const auto tables = render_report_tables(files);
  ASSERT_TRUE(tables.has_value()) << tables.error();
  EXPECT_NE(tables->find("Derived from the option chain: yes."), std::string::npos) << *tables;
  const std::string block = "\n    A synthetic attribution.\n";
  const auto at = tables->find(block);
  ASSERT_NE(at, std::string::npos) << *tables;
  EXPECT_EQ(tables->find(block, at + 1), std::string::npos) << *tables;
  for (const std::string_view fragment : kAttributionFragments) {
    EXPECT_NE(tables->find("    (" + std::string(fragment) + ")\n"), std::string::npos) << fragment;
  }
  EXPECT_EQ(render_report_tables(std::vector<ResultsFile>{stamped_report()})->find("attribution"),
            std::string::npos);
}

TEST(ReportTables, AHostTableOfReportsHasNoBenchmarkTable) {
  const std::vector<ResultsFile> files = {stamped_report()};
  const auto table = render_host_table(files);
  ASSERT_TRUE(table.has_value()) << table.error();
  EXPECT_EQ(table->find("| benchmark |"), std::string::npos) << *table;
  EXPECT_NE(table->find("#### Fit error per slice"), std::string::npos) << *table;
}

TEST(ReportTables, AMixedHostTableHasTheBenchmarksThenTheReports) {
  const std::vector<ResultsFile> files = {stamped_report(), stamped()};
  const auto table = render_host_table(files);
  ASSERT_TRUE(table.has_value()) << table.error();
  const auto benchmarks = table->find("| harness_selftest |");
  const auto reports = table->find("#### Fit error per slice");
  ASSERT_NE(benchmarks, std::string::npos) << *table;
  ASSERT_NE(reports, std::string::npos) << *table;
  EXPECT_LT(benchmarks, reports);
}

TEST(ReportTables, RefuseAReportThatIsNotReportableUnlessIteration) {
  const std::vector<ResultsFile> files = {stamped_report(true, false)};
  EXPECT_FALSE(render_report_tables(files).has_value());
  const auto iteration = render_host_table(files, {.iteration = true, .host_heading = true});
  ASSERT_TRUE(iteration.has_value()) << iteration.error();
  EXPECT_NE(iteration->find("ITERATION ONLY"), std::string::npos) << *iteration;
}

TEST(ReportTables, RefuseAReportEditedOutOfTheGrammar) {
  ResultsFile file = stamped_report();
  file.sections.front().body += "row s3 inf 1 x\n";
  const std::vector<ResultsFile> files = {file};
  EXPECT_FALSE(render_report_tables(files).has_value());
  EXPECT_FALSE(render_host_table(files).has_value());
}

}  // namespace
}  // namespace aadp::results
