// The AAD tables (results/aad_table.hpp) and the iteration mode of the results table.
#include "results/aad_table.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fixtures.hpp"
#include "results/results_file.hpp"
#include "results/table.hpp"

namespace aadp::results {
namespace {

constexpr std::string_view kSection =
    "workload w\ntargets AAD-4\ndescription an Asian\ndifferences none\nkind pricer\n"
    "paths 1000\nbatches 50\nthreads 1\nseed 20261104\nsteps 12\ninputs 5\n"
    "tape_statements_per_path 63\ntape_bytes_per_path 1239\nnonzero_payoff_fraction 0.5\n"
    "valid yes\n"
    "check value-bits pass 0\ncheck central-vs-tape pass 0.25\n"
    "peak_rss price 2097152\npeak_rss tape 4194304\n"
    "variant price w.price\nvariant tape w.tape\nvariant tape-record w.tape-record\n"
    "variant bump-central w.bump-central\n"
    "estimate price value 5.5 0.25\n"
    "estimate tape delta 0.5 0.01\n"
    "estimate bump-central delta 0.5 0.01\n"
    "step central delta 0.0001 0.001 1e-05 0.002\n";

std::string benchmark_json() {
  std::string entries =
      "{\"run_name\": \"harness_selftest/manual_time\", \"aggregate_name\": \"mean\", "
      "\"real_time\": 4100.5}";
  for (const auto& [name, ns] :
       std::vector<std::pair<std::string_view, std::string_view>>{{"w.price", "1e6"},
                                                                  {"w.tape", "4e6"},
                                                                  {"w.tape-record", "3e6"},
                                                                  {"w.bump-central", "11e6"}}) {
    entries += ", {\"run_name\": \"" + std::string(name) +
               "/iterations:1/repeats:3/manual_time\", \"aggregate_name\": \"mean\", "
               "\"real_time\": " +
               std::string(ns) + "}";
  }
  return "{\n  \"context\": {\"date\": \"2026-01-02T03:04:06+00:00\"},\n  \"benchmarks\": [" +
         entries + "]\n}\n";
}

ResultsFile aad_file(std::string_view section, bool reportable = true,
                     bool through_measure = true) {
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "claim", "result");
  for (ResultsSection& s : raw.sections) {
    if (s.name == kBenchmarkSection) s.body = benchmark_json();
  }
  raw.sections.push_back({.name = "aad_w", .body = std::string(section)});
  testing::StampCase test_case;
  test_case.raw = testing::text_of(raw);
  if (!through_measure) test_case.captured.clear();
  test_case.request.reportable = reportable;
  test_case.request.not_reportable_reason = reportable ? "" : "iteration run";
  const auto stamped = test_case.run();
  EXPECT_TRUE(stamped.has_value()) << stamped.error().detail;
  return *parse_results_file(*stamped);
}

TEST(AadTable, RendersEachVariantAgainstTheScalarPrimal) {
  const std::vector<ResultsFile> files = {aad_file(kSection)};
  const auto table = render_host_table(files);
  ASSERT_TRUE(table.has_value()) << table.error();
  // time 4 ms, 1000 paths in 4 ms, 4 times the primal, the SIMD slot, 4 MiB, twice the primal's.
  EXPECT_NE(table->find("| tape | 4 | 2.5e+05 | 4 | pending stage 5 | 4.0 | 2.00 | 1239 |"),
            std::string::npos)
      << *table;
  EXPECT_NE(table->find("reverse sweep (tape - tape-record) 1 ms, 1 x primal"), std::string::npos);
  EXPECT_NE(table->find("| price | 1 | 1e+06 | 1 |"), std::string::npos);
  EXPECT_NE(table->find("11 pricings expected (2N + 1)"), std::string::npos);
  EXPECT_NE(table->find("nonzero-payoff fraction 0.5"), std::string::npos);
  EXPECT_EQ(table->find("ITERATION ONLY"), std::string::npos);
}

TEST(AadTable, PutsTheStudyBiasAndRmseTimesTimeBesideEachGreek) {
  const std::vector<ResultsFile> files = {aad_file(kSection)};
  const auto table = render_host_table(files);
  ASSERT_TRUE(table.has_value()) << table.error();
  // (0.01^2 + 0.001^2) 0.011 s and 0.01^2 0.004 s.
  EXPECT_NE(
      table->find("| delta | bump-central | 0.5 | 0.01 | 0.001 | 1e-05 | 0.0001 | 1.11e-06 |"),
      std::string::npos)
      << *table;
  EXPECT_NE(table->find("| delta | tape | 0.5 | 0.01 | 0 (pathwise) | - | - | 4e-07 |"),
            std::string::npos);
}

TEST(AadTable, MarksAWorkloadWhoseCheckFailedInvalid) {
  std::string section(kSection);
  section.replace(section.find("check central-vs-tape pass"), 26, "check central-vs-tape fail");
  const std::vector<ResultsFile> files = {aad_file(section)};
  const auto table = render_host_table(files);
  ASSERT_TRUE(table.has_value()) << table.error();
  EXPECT_NE(table->find("##### w (AAD-4): INVALID"), std::string::npos) << *table;
  EXPECT_NE(table->find("central-vs-tape FAIL"), std::string::npos);
  EXPECT_NE(table->find("| tape | 4 | 2.5e+05 | INVALID | INVALID |"), std::string::npos);
}

TEST(AadTable, ASectionThatSaysInvalidIsInvalidWhateverItsChecksSay) {
  std::string section(kSection);
  section.replace(section.find("valid yes"), 9, "valid no");
  const std::vector<ResultsFile> files = {aad_file(section)};
  EXPECT_NE(render_host_table(files)->find(": INVALID"), std::string::npos);
}

TEST(AadTable, RefusesAVariantWithNoBenchmarkEntry) {
  std::string section(kSection);
  section += "variant hand w.hand\n";
  const std::vector<ResultsFile> files = {aad_file(section)};
  const auto table = render_host_table(files);
  ASSERT_FALSE(table.has_value());
  EXPECT_NE(table.error().find("no benchmark entry for w.hand"), std::string::npos);
}

TEST(AadTable, RefusesAMalformedLine) {
  std::string section(kSection);
  section += "estimate tape vega x 0.1\n";
  const std::vector<ResultsFile> files = {aad_file(section)};
  EXPECT_FALSE(render_host_table(files).has_value());
}

TEST(IterationTable, AdmitsAFileThatIsNotReportableUnderABanner) {
  const std::vector<ResultsFile> files = {aad_file(kSection, false, false)};
  EXPECT_FALSE(render_host_table(files).has_value());
  const auto table = render_host_table(files, {.iteration = true});
  ASSERT_TRUE(table.has_value()) << table.error();
  EXPECT_LT(table->find("**ITERATION ONLY: not results.** Not reportable: iteration run."),
            table->find("| benchmark |"))
      << *table;
  EXPECT_NE(table->find("#### AAD cost and accuracy (ITERATION ONLY: not results)"),
            std::string::npos);
  // No measure.sh, so no load.
  EXPECT_NE(table->find("| 90000 | absent | absent |"), std::string::npos);
}

TEST(IterationTable, AReportableFileRendersAsWithoutIt) {
  const std::vector<ResultsFile> files = {aad_file(kSection)};
  EXPECT_EQ(*render_host_table(files, {.iteration = true}), *render_host_table(files));
}

}  // namespace
}  // namespace aadp::results
