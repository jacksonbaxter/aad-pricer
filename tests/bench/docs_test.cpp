// RESULTS.md and the README's headline (results/docs.hpp), from synthetic files.
#include "results/docs.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fixtures.hpp"
#include "results/results_file.hpp"

namespace aadp::results {
namespace {

constexpr std::string_view kSpec =
    "# a comment\n"
    "kind,id,host,source,detail,target,target_text,better,scale,unit,label\n"
    "measured_run,run 1,linux-zen1,references/r.csv,,,,,,,\n"
    "multiple,AAD-1,linux-zen1,w,tape/price,3.0,below 3.0,below,1,,times one pricing\n"
    "multiple,AAD-5,linux-zen1,w,tape/hand,1.25,within 1.25,at_most,1,,times the hand adjoint\n"
    "paths_per_second,MC-1,linux-zen1,w,price,1.044e6,1.044 million,at_least,1,paths/s,\n"
    "largest,MEM-4,linux-zen1,tape_allocated_bytes,,50,below 50 MB,below,1e6,MB,of tape\n"
    "oracle_run,run 1,linux-zen1,references/r.csv,,,,,,,\n";

constexpr std::string_view kBenchmarks =
    "# BENCHMARKS\n\n### AAD-1: barrier: multiple below 3.0\n\n"
    "### AAD-5: within 1.25 times a hand-written adjoint\n\n"
    "### MC-1: Asian: 1.044 million paths/s, single thread\n\n"
    "### MEM-4: per-path tape below 50 MB in the worst case\n";

constexpr std::string_view kRun =
    "# Measured run 1. Host linux-zen1, Linux.\n"
    "# run=3 passed=2 failed=1 skipped=0.\n"
    "id,estimate,reference,se,bound,verdict\n"
    "A.value,1.0,1.0,0.1,0.5,pass\n"
    "A.delta,0.5,0.4,0.01,0.05,FAIL\n"
    "A.vega,2.0,2.0,0.1,0.5,pass\n";

constexpr std::string_view kFaultReport =
    "aadp-fault-injection 1\nclaim: none\ncommit: 0123456789abcdef\nhost_label: linux-zen1\n"
    "utc_time: 2026-01-02T03:04:05Z\nselected: 2\nscope: all 2 faults\n"
    "columns: id | covers | baseline | mutated_exit | reason | verdict\n"
    "a | x | pass | 1 | r | caught\nb | y | pass | 1 | r | caught\n"
    "summary: caught=2 not-caught=0\n";

// price 2 ms, tape 4 ms and hand 2.5 ms per run of 1000 paths.
std::string section(std::string_view targets = "AAD-1, AAD-5, MC-1", bool valid = true) {
  return "workload w\ntargets " + std::string(targets) +
         "\ndescription d\ndifferences none\nkind pricer\npaths 1000\nbatches 50\nthreads 1\n"
         "seed 20261104\nsteps 12\ninputs 5\ntape_statements_per_path 63\n"
         "tape_bytes_per_path 1239\ntape_allocated_bytes 46000000\n"
         "nonzero_payoff_fraction 0.5\nvalid yes\ncheck value-bits " +
         (valid ? "pass" : "fail") +
         " 0\npeak_rss price 2097152\npeak_rss tape 4194304\n"
         "variant price w.price\nvariant tape w.tape\nvariant hand w.hand\n"
         "estimate price value 5.5 0.25\nestimate tape delta 0.5 0.01\n";
}

std::string benchmark_json(std::string_view price_ns) {
  std::string entries = R"({"run_name": "harness_selftest/manual_time", "aggregate_name": "mean", )"
                        R"("real_time": 4100.5})";
  const std::vector<std::pair<std::string_view, std::string_view>> times = {
      {"w.price", price_ns}, {"w.tape", "4e6"}, {"w.hand", "2.5e6"}};
  for (const auto& [name, ns] : times) {
    entries += R"(, {"run_name": ")" + std::string(name) +
               R"(/iterations:1/repeats:3/manual_time", "aggregate_name": "mean", "real_time": )" +
               std::string(ns) + "}";
  }
  return "{\n  \"context\": {\"date\": \"2026-01-02T03:04:06+00:00\"},\n  \"benchmarks\": [" +
         entries + "]\n}\n";
}

std::string results_text(std::string_view label, const std::string& aad,
                         std::string_view started = "2026-01-02T03:04:06Z",
                         std::string_view price_ns = "2e6") {
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "claim", "result");
  testing::set_field(raw, "host_label", label);
  testing::set_field(raw, "os", label == "mac-m4max" ? "macOS 27.2" : "Linux, synthetic");
  testing::set_field(raw, "run_started_at", started);
  for (ResultsSection& s : raw.sections) {
    if (s.name == kBenchmarkSection) {
      s.body = benchmark_json(price_ns);
    }
  }
  raw.sections.push_back({.name = "aad_w", .body = aad});
  testing::StampCase test_case;
  test_case.raw = testing::text_of(raw);
  test_case.request.current_host_label = std::string(label);
  const auto stamped = test_case.run();
  EXPECT_TRUE(stamped.has_value()) << stamped.error().detail;
  return stamped.value_or("");
}

DocsInputs inputs() {
  DocsInputs in;
  in.spec = kSpec;
  in.benchmarks = kBenchmarks;
  in.files.push_back(
      {.host_dir = "linux-zen1", .name = "a.txt", .text = results_text("linux-zen1", section())});
  in.files.push_back({.host_dir = "linux-zen1",
                      .name = "fault-injection-1.txt",
                      .text = std::string(kFaultReport)});
  in.measured_runs["references/r.csv"] = kRun;
  return in;
}

bool has(const std::string& text, std::string_view part) { return text.contains(part); }

TEST(Docs, TheHeadlineComesFromTheFiles) {
  const auto docs = render_docs(inputs());
  ASSERT_TRUE(docs.has_value()) << docs.error();
  const std::string& h = docs->headline;
  EXPECT_TRUE(has(h, "| target | measured | host | verdict |\n")) << h;
  EXPECT_TRUE(has(h,
                  "| AAD-1: barrier: multiple below 3.0 | 2 times one pricing | "
                  "[linux-zen1](results/linux-zen1/a.txt), Linux | met, by 1.00 |"))
      << h;
  EXPECT_TRUE(has(h,
                  "| 1.6 times the hand adjoint | [linux-zen1](results/linux-zen1/a.txt), "
                  "Linux | missed, by 0.35 |"))
      << h;
  // 1000 paths in 2 ms; 1.044e6 / 5e5 = 2.088.
  EXPECT_TRUE(has(h,
                  "| 500,000 paths/s | [linux-zen1](results/linux-zen1/a.txt), Linux | "
                  "missed, by 2.1 times |"))
      << h;
  EXPECT_TRUE(has(h, "| 46 MB of tape (`w`) |")) << h;
  EXPECT_TRUE(has(h, "met, by 4.00 MB |")) << h;
  EXPECT_TRUE(has(h, "| Oracle plan, run 1: all 3 registered comparisons pass the tolerance rule"))
      << h;
  EXPECT_TRUE(has(h, "| 2 of 3 pass | [linux-zen1](references/r.csv), Linux | 1 failed: A.delta |"))
      << h;
  EXPECT_TRUE(has(h, "measured at commit `0123456`.")) << h;
}

TEST(Docs, ResultsMdHoldsEachHostApartAndTheRunsSideBySide) {
  DocsInputs in = inputs();
  in.files.push_back(
      {.host_dir = "mac-m4max", .name = "m.txt", .text = results_text("mac-m4max", section())});
  in.measured_runs["references/s.csv"] =
      "id,estimate,reference,se,bound,verdict\nA.value,1.5,1.0,0.1,0.5,pass\n";
  in.spec += "measured_run,run 2,linux-zen1,references/s.csv,,,,,,,\n";
  const auto docs = render_docs(in);
  ASSERT_TRUE(docs.has_value()) << docs.error();
  const std::string& r = docs->results_md;
  const auto zen = r.find("\n## linux-zen1\n");
  const auto mac = r.find("\n## mac-m4max\n");
  ASSERT_NE(zen, std::string::npos);
  ASSERT_NE(mac, std::string::npos);
  EXPECT_LT(zen, mac);
  EXPECT_LT(r.find("[a.txt](results/linux-zen1/a.txt)"), mac);
  EXPECT_GT(r.find("[m.txt](results/mac-m4max/m.txt)"), mac);
  EXPECT_TRUE(has(r,
                  "| [fault-injection-1.txt](results/linux-zen1/fault-injection-1.txt) | "
                  "`0123456` | 2026-01-02T03:04:05Z | 2 | all 2 faults | caught=2 "
                  "not-caught=0 |"))
      << r;
  // Run 2 lists its row first; run 1's other rows follow, and run 2 did not run them.
  EXPECT_TRUE(has(r, "| A.value | 1.0 | 1.0 | 0.1 | 0.5 | pass | 1.5 | 1.0 | 0.1 | 0.5 | pass |"))
      << r;
  EXPECT_TRUE(has(r, "| A.delta | 0.5 | 0.4 | 0.01 | 0.05 | FAIL | - | - | - | - | not run |"))
      << r;
  EXPECT_FALSE(has(r, "    # run=3"));
  EXPECT_TRUE(has(r, "    run=3 passed=2 failed=1 skipped=0.\n")) << r;
  EXPECT_TRUE(has(r,
                  "| run 1 | [references/r.csv](references/r.csv) | linux-zen1, Linux | 3 | 2 | "
                  "1 |"))
      << r;
  EXPECT_TRUE(has(r, "| target | measured | host | verdict | computed as |")) << r;
}

TEST(Docs, TheLatestRunOfAWorkloadAnswersItsTarget) {
  DocsInputs in = inputs();
  in.files.push_back(
      {.host_dir = "linux-zen1",
       .name = "b.txt",
       .text = results_text("linux-zen1", section(), "2026-01-02T03:04:08Z", "1e6")});
  const auto docs = render_docs(in);
  ASSERT_TRUE(docs.has_value()) << docs.error();
  EXPECT_TRUE(has(docs->headline, "| 4 times one pricing | [linux-zen1](results/linux-zen1/b.txt)"))
      << docs->headline;
}

TEST(Docs, RefusesTwoRunsOfAWorkloadStartedTogether) {
  DocsInputs in = inputs();
  in.files.push_back(
      {.host_dir = "linux-zen1", .name = "b.txt", .text = results_text("linux-zen1", section())});
  const auto docs = render_docs(in);
  ASSERT_FALSE(docs.has_value());
  EXPECT_TRUE(has(docs.error(), "two runs of w started at")) << docs.error();
}

TEST(Docs, RefusesATargetItsHeadingDoesNotState) {
  DocsInputs in = inputs();
  in.benchmarks = "### AAD-1: barrier: multiple below 2.0\n" + std::string(kBenchmarks);
  const auto docs = render_docs(in);
  ASSERT_FALSE(docs.has_value());
  EXPECT_TRUE(has(docs.error(), "the heading of AAD-1 does not say below 3.0")) << docs.error();
}

TEST(Docs, RefusesAWorkloadThatDoesNotNameItsTarget) {
  DocsInputs in = inputs();
  in.files[0].text = results_text("linux-zen1", section("AAD-4, MC-1"));
  const auto docs = render_docs(in);
  ASSERT_FALSE(docs.has_value());
  EXPECT_TRUE(has(docs.error(), "w's targets do not name AAD-1")) << docs.error();
}

TEST(Docs, MarksAWorkloadWhoseChecksFailedInvalid) {
  DocsInputs in = inputs();
  in.files[0].text = results_text("linux-zen1", section("AAD-1, AAD-5, MC-1", false));
  in.spec = std::string(kSpec).substr(0, std::string(kSpec).find("largest")) +
            "oracle_run,run 1,linux-zen1,references/r.csv,,,,,,,\n";
  const auto docs = render_docs(in);
  ASSERT_TRUE(docs.has_value()) << docs.error();
  EXPECT_TRUE(has(docs->headline,
                  "| INVALID | [linux-zen1](results/linux-zen1/a.txt), Linux | "
                  "INVALID: the run's in-run checks did not all pass |"))
      << docs->headline;
  EXPECT_FALSE(has(docs->headline, "met, by"));
}

TEST(Docs, RefusesAFileUnderAnotherHostsDirectory) {
  DocsInputs in = inputs();
  in.files[0].host_dir = "mac-m4max";
  const auto docs = render_docs(in);
  ASSERT_FALSE(docs.has_value());
  EXPECT_TRUE(has(docs.error(), "mac-m4max/a.txt is under mac-m4max but its header says otherwise"))
      << docs.error();
}

TEST(Docs, RefusesARunWhoseFileNamesAnotherHost) {
  DocsInputs in = inputs();
  in.measured_runs["references/r.csv"] = "# Host mac-m4max, macOS.\n" + std::string(kRun);
  const auto docs = render_docs(in);
  ASSERT_FALSE(docs.has_value());
  EXPECT_TRUE(has(docs.error(), "references/r.csv names host mac-m4max, not linux-zen1"))
      << docs.error();
}

TEST(HeadlineSpec, RefusesWhatIsNotASpec) {
  const std::string header(kHeadlineSpecHeader);
  EXPECT_FALSE(parse_headline_spec("kind,id\n").has_value());
  EXPECT_FALSE(parse_headline_spec(header + "\n").has_value());  // no rows
  EXPECT_FALSE(parse_headline_spec(header + "\nbest,AAD-1,linux-zen1,w,tape/price,3,3,below,1,,\n")
                   .has_value());
  EXPECT_FALSE(parse_headline_spec(header + "\nmultiple,AAD-1,linux-zen1,w,tape,3,3,below,1,,\n")
                   .has_value());
  EXPECT_FALSE(
      parse_headline_spec(header + "\nmultiple,AAD-1,linux-zen1,w,tape/price,3,3,under,1,,\n")
          .has_value());
  EXPECT_FALSE(
      parse_headline_spec(header + "\nmultiple,AAD-1,linux-zen1,w,tape/price,x,3,below,1,,\n")
          .has_value());
  EXPECT_FALSE(
      parse_headline_spec(header + "\nmultiple,AAD-1,linux-zen1,w,tape/price,3,3,below,0,,\n")
          .has_value());
  // An oracle run that is not also a measured run, and a path out of references/.
  EXPECT_FALSE(
      parse_headline_spec(header + "\noracle_run,run 1,linux-zen1,references/r.csv,,,,,,,\n")
          .has_value());
  EXPECT_FALSE(parse_headline_spec(header +
                                   "\nmeasured_run,run 1,linux-zen1,references/../r.csv,,,,,,,\n"
                                   "multiple,AAD-1,linux-zen1,w,tape/price,3,3,below,1,,\n")
                   .has_value());
  const auto spec = parse_headline_spec(kSpec);
  ASSERT_TRUE(spec.has_value()) << spec.error();
  EXPECT_EQ(spec->measured_runs.size(), 1U);
  EXPECT_EQ(spec->rows.size(), 5U);
  EXPECT_EQ(spec->rows[2].target, 1.044e6);
  EXPECT_EQ(spec->rows[3].scale, 1e6);
}

TEST(MeasuredRun, ReadsTheRowsTheHostAndTheCounts) {
  const auto run = parse_measured_run(kRun);
  ASSERT_TRUE(run.has_value()) << run.error();
  EXPECT_EQ(run->rows.size(), 3U);
  EXPECT_EQ(run->passed, 2U);
  EXPECT_EQ(run->host, "linux-zen1");
  EXPECT_EQ(run->comments.front(), "Measured run 1. Host linux-zen1, Linux.");
}

TEST(MeasuredRun, RefusesASummaryTheRowsContradict) {
  std::string text(kRun);
  text.replace(text.find("passed=2"), 8, "passed=3");
  const auto run = parse_measured_run(text);
  ASSERT_FALSE(run.has_value());
  EXPECT_TRUE(has(run.error(), "says passed=3 but the rows give 2")) << run.error();
}

TEST(MeasuredRun, RefusesMalformedRows) {
  const std::string header = std::string(kMeasuredRunHeader) + "\n";
  EXPECT_FALSE(parse_measured_run(header).has_value());
  EXPECT_FALSE(parse_measured_run(header + "A,1,1,1,1,maybe\n").has_value());
  EXPECT_FALSE(parse_measured_run(header + "A,1,1,1,pass\n").has_value());
  EXPECT_FALSE(parse_measured_run(header + "A,1,1,1,1,pass\nA,1,1,1,1,pass\n").has_value());
  EXPECT_FALSE(parse_measured_run("id,x\nA,1,1,1,1,pass\n").has_value());
  EXPECT_TRUE(parse_measured_run(header + "A,1,1,1,1,pass\n").has_value());
}

TEST(FaultReport, ReadsTheHeaderTheFaultsAndTheSummary) {
  const auto report = parse_fault_report(kFaultReport);
  ASSERT_TRUE(report.has_value()) << report.error();
  EXPECT_EQ(report->faults, 2U);
  EXPECT_EQ(report->summary, "caught=2 not-caught=0");
  EXPECT_EQ(report->header.at("scope"), "all 2 faults");
  std::string no_summary(kFaultReport);
  no_summary.resize(no_summary.find("summary:"));
  EXPECT_FALSE(parse_fault_report(no_summary).has_value());
  EXPECT_FALSE(parse_fault_report("aadp-fault-injection 1\ncolumns: x\nsummary: y\n").has_value());
}

TEST(Headline, SplicesBetweenTheMarkersOnly) {
  const std::string begin(kHeadlineBegin);
  const std::string end(kHeadlineEnd);
  const std::string readme = "# r\n\n" + begin + "\nold\n" + end + "\n\nrest\n";
  const auto spliced = splice_headline(readme, "new\n");
  ASSERT_TRUE(spliced.has_value()) << spliced.error();
  EXPECT_EQ(*spliced, "# r\n\n" + begin + "\nnew\n" + end + "\n\nrest\n");
  EXPECT_FALSE(splice_headline("# r\n", "new\n").has_value());
  EXPECT_FALSE(splice_headline(end + "\n" + begin + "\n", "new\n").has_value());
  EXPECT_FALSE(splice_headline(readme + begin + "\n", "new\n").has_value());
}

TEST(Headline, FindsTheTargetHeadingAndTheFirstDifference) {
  EXPECT_EQ(target_heading(kBenchmarks, "MC-1"),
            "MC-1: Asian: 1.044 million paths/s, single thread");
  EXPECT_FALSE(target_heading(kBenchmarks, "MC-2").has_value());
  EXPECT_FALSE(target_heading(kBenchmarks, "MC").has_value());
  EXPECT_EQ(first_difference_line("a\nb\n", "a\nb\n"), 0U);
  EXPECT_EQ(first_difference_line("a\nb\n", "a\nc\n"), 2U);
  EXPECT_EQ(first_difference_line("a\n", "a\nb\n"), 2U);
}

}  // namespace
}  // namespace aadp::results
