#include "results/table.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "fixtures.hpp"

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
}

}  // namespace
}  // namespace aadp::results
