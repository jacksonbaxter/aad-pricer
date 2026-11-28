// RESULTS.md and the README's headline (results/docs.hpp), from synthetic files.
#include "results/docs.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
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

// One row per verdict the "Measured" lines of inputs() state; AAD-1's note holds a comma.
constexpr std::string_view kVerdicts =
    "# a comment\n"
    "target,host,condition,verdict,basis,files,headline,withdrawn,rerun,note\n"
    "AAD-1,linux-zen1,,met,files,results/linux-zen1/a.txt,w tape/price,,,a note, with a comma\n"
    "AAD-5,linux-zen1,,missed,files,results/linux-zen1/a.txt,w tape/hand,,,\n"
    "MC-1,linux-zen1,,missed,files,results/linux-zen1/a.txt,w price,,,\n"
    "MEM-4,linux-zen1,,met,files,results/linux-zen1/a.txt,tape_allocated_bytes,,,\n";

// Puts a "Measured" line just under the target's heading.
void state(std::string& benchmarks, std::string_view id, std::string_view text) {
  const std::size_t heading = benchmarks.find("### " + std::string(id) + ":");
  ASSERT_NE(heading, std::string::npos) << id;
  benchmarks.insert(benchmarks.find('\n', heading) + 1, "- Measured " + std::string(text) + "\n");
}

std::string stated_benchmarks(std::string_view aad1 = "met") {
  std::string out(kBenchmarks);
  state(out, "AAD-1", "on `linux-zen1`: " + std::string(aad1) + ".");
  state(out, "AAD-5", "on `linux-zen1`: missed.");
  state(out, "MC-1", "on `linux-zen1`: missed.");
  state(out, "MEM-4", "on `linux-zen1`: met.");
  return out;
}

// AAD-1 judged missed: its "Measured" line and its row.
void judge_aad1_missed(DocsInputs& in) {
  in.benchmarks = stated_benchmarks("missed");
  in.verdicts.replace(in.verdicts.find("AAD-1,linux-zen1,,met,"), 22, "AAD-1,linux-zen1,,missed,");
}

constexpr std::string_view kRun =
    "# Measured run 1. Host linux-zen1, Linux.\n"
    "# Code of 1234567 run at commit 89abcde, seed 1.\n"
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
                         std::string_view price_ns = "2e6",
                         std::pair<std::string_view, std::string_view> build = {"baseline",
                                                                                "SSE2"}) {
  ResultsFile raw = testing::raw_file();
  testing::set_field(raw, "claim", "result");
  testing::set_field(raw, "host_label", label);
  testing::set_field(raw, "os", label == "mac-m4max" ? "macOS 27.2" : "Linux, synthetic");
  testing::set_field(raw, "run_started_at", started);
  testing::set_field(raw, "simd_cmake_target", build.first);
  testing::set_field(raw, "simd_target", build.second);
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
  in.verdicts = kVerdicts;
  in.benchmarks = stated_benchmarks();
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
}

// The results files' rows were measured at 0123456 and the oracle run at 89abcde, which its own
// file names; no commit is said of a row whose file does not give it.
TEST(Docs, EachRowSaysTheCommitItsSourceFileGives) {
  const auto docs = render_docs(inputs());
  ASSERT_TRUE(docs.has_value()) << docs.error();
  EXPECT_TRUE(has(docs->headline,
                  " Each row's commit, as its source file says: `0123456` for AAD-1, AAD-5, MC-1 "
                  "and MEM-4; `89abcde` for the oracle plan's run 1.\n"))
      << docs->headline;

  DocsInputs in = inputs();
  std::string& run = in.measured_runs["references/r.csv"];
  run.erase(run.find("# Code of"), run.find("# run=") - run.find("# Code of"));
  const auto none = render_docs(in);
  ASSERT_FALSE(none.has_value());
  EXPECT_TRUE(has(none.error(), "references/r.csv names no commit it ran at")) << none.error();

  run.insert(0, "# Code run at commit 89abcdX.\n");
  const auto bad = render_docs(in);
  ASSERT_FALSE(bad.has_value());
  EXPECT_TRUE(has(bad.error(), "does not name a commit")) << bad.error();
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
  EXPECT_TRUE(has(r, "| harness_selftest | `baseline`, SSE2 | 3 |")) << r;
  EXPECT_TRUE(has(r, "the `release` preset builds `baseline` and `release-avx2` builds `avx2`"))
      << r;
}

TEST(Docs, TheLatestRunOfAWorkloadAnswersItsTarget) {
  DocsInputs in = inputs();
  in.files.push_back(
      {.host_dir = "linux-zen1",
       .name = "b.txt",
       .text = results_text("linux-zen1", section(), "2026-01-02T03:04:08Z", "1e6")});
  judge_aad1_missed(in);
  const auto docs = render_docs(in);
  ASSERT_TRUE(docs.has_value()) << docs.error();
  EXPECT_TRUE(has(docs->headline, "| 4 times one pricing | [linux-zen1](results/linux-zen1/b.txt)"))
      << docs->headline;
}

// a.txt is the baseline build's run; b.txt, later, is another build's. A row naming the baseline
// build takes a.txt, a row naming none takes b.txt, and a build no file has is refused.
TEST(Docs, AWorkloadRowCanNameItsBuild) {
  DocsInputs in = inputs();
  in.files.push_back({.host_dir = "linux-zen1",
                      .name = "b.txt",
                      .text = results_text("linux-zen1", section(), "2026-01-02T03:04:08Z", "1e6",
                                           {"avx2", "AVX2"})});
  const std::string bare = in.spec;
  const auto replace_source = [&bare](std::string_view source) {
    std::string spec = bare;
    for (std::size_t at = spec.find(",linux-zen1,w,"); at != std::string::npos;
         at = spec.find(",linux-zen1,w,", at + 1)) {
      spec.replace(at, 14, ",linux-zen1," + std::string(source) + ",");
    }
    return spec;
  };
  in.spec = replace_source("w@baseline");
  const std::string verdicts = in.verdicts;
  for (std::size_t at = in.verdicts.find(",w "); at != std::string::npos;
       at = in.verdicts.find(",w ", at + 1)) {
    in.verdicts.replace(at, 3, ",w@baseline ");
  }
  const auto docs = render_docs(in);
  ASSERT_TRUE(docs.has_value()) << docs.error();
  EXPECT_TRUE(has(docs->headline, "| 2 times one pricing | [linux-zen1](results/linux-zen1/a.txt)"))
      << docs->headline;
  EXPECT_TRUE(has(docs->headline, "| 500,000 paths/s | [linux-zen1](results/linux-zen1/a.txt)"))
      << docs->headline;
  EXPECT_TRUE(has(docs->results_md,
                  "mean time of `tape` over mean time of `price` on `w` in the `baseline` build "
                  "(SSE2), each the mean of"))
      << docs->results_md;

  in.spec = bare;
  in.verdicts = verdicts;
  judge_aad1_missed(in);  // b.txt's AAD-1 is 4
  const auto latest = render_docs(in);
  ASSERT_TRUE(latest.has_value()) << latest.error();
  EXPECT_TRUE(
      has(latest->headline, "| 4 times one pricing | [linux-zen1](results/linux-zen1/b.txt)"))
      << latest->headline;

  in.spec = replace_source("w@avx512");
  const auto none = render_docs(in);
  ASSERT_FALSE(none.has_value());
  EXPECT_TRUE(has(none.error(), "no result on this host has workload w in build avx512"))
      << none.error();
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
  // An INVALID run has no verdict: its lines state none and its rows say none.
  in.benchmarks = kBenchmarks;
  for (const std::string_view id : {"AAD-1", "AAD-5", "MC-1"}) {
    state(in.benchmarks, id, "on `linux-zen1`: INVALID.");
  }
  state(in.benchmarks, "MEM-4", "on `linux-zen1`: met.");
  in.verdicts = std::string(kVerdictsHeader) +
                "\nAAD-1,linux-zen1,,none,files,results/linux-zen1/a.txt,w tape/price,,,\n"
                "AAD-5,linux-zen1,,none,files,results/linux-zen1/a.txt,w tape/hand,,,\n"
                "MC-1,linux-zen1,,none,files,results/linux-zen1/a.txt,w price,,,\n"
                "MEM-4,linux-zen1,,met,files,results/linux-zen1/a.txt,,,,\n";
  const auto docs = render_docs(in);
  ASSERT_TRUE(docs.has_value()) << docs.error();
  EXPECT_TRUE(has(docs->headline,
                  "| INVALID | [linux-zen1](results/linux-zen1/a.txt), Linux | "
                  "INVALID: the run's in-run checks did not all pass |"))
      << docs->headline;
  EXPECT_FALSE(has(docs->headline, "met, by"));
  EXPECT_TRUE(has(docs->headline,
                  "Measured with no verdict stated: [AAD-1](BENCHMARKS.md#aad-1-barrier-multiple-"
                  "below-30), [AAD-5]"))
      << docs->headline;

  // A row that calls an INVALID run met disagrees with the headline.
  state(in.benchmarks, "AAD-1", "on `linux-zen1`: met.");
  in.verdicts.replace(in.verdicts.find("AAD-1,linux-zen1,,none,"), 23, "AAD-1,linux-zen1,,met,");
  const auto met = render_docs(in);
  ASSERT_FALSE(met.has_value());
  EXPECT_TRUE(has(met.error(), "computes none, and results/verdicts.csv says met")) << met.error();
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

// --- report files and the report_cell headline ---

std::string report_text(std::string_view started = "2026-01-02T03:04:06Z",
                        std::string_view error = "0.25", bool uses_chain = false,
                        std::string_view claim = "result") {
  std::string body(testing::kReportBody);
  body.replace(body.find("row s1 0.25"), 11, "row s1 " + std::string(error));
  if (uses_chain) {
    body.replace(body.find("uses_chain no"), 13, "uses_chain yes");
  }
  ResultsFile raw = testing::raw_report_file(body);
  testing::set_field(raw, "claim", claim);
  testing::set_field(raw, "run_started_at", started);
  if (uses_chain) {
    raw.sections.insert(raw.sections.begin(),
                        {.name = "attribution", .body = testing::attribution_body()});
  }
  testing::StampCase test_case;
  test_case.raw = testing::text_of(raw);
  const auto stamped = test_case.run();
  EXPECT_TRUE(stamped.has_value()) << stamped.error().detail;
  return stamped.value_or("");
}

constexpr std::string_view kReportRow =
    "report_cell,SVI-6,linux-zen1,fit_error,s1/error,4,at most 4 bp,at_most,1,bp of F,on s1\n";
constexpr std::string_view kReportHeading = "\n### SVI-6: fit error at most 4 bp of forward\n";

constexpr std::string_view kReportVerdict =
    "SVI-6,linux-zen1,,met,files,results/linux-zen1/r1.txt,fit_error s1/error,,,\n";

DocsInputs report_inputs(std::string_view row = kReportRow,
                         std::string_view verdict = kReportVerdict, std::string_view word = "met") {
  DocsInputs in = inputs();
  in.spec += row;
  in.verdicts += verdict;
  in.benchmarks += kReportHeading;
  in.benchmarks += "- Measured on `linux-zen1`: " + std::string(word) + ".\n";
  in.files.push_back({.host_dir = "linux-zen1", .name = "r1.txt", .text = report_text()});
  return in;
}

TEST(DocsReport, AReportCellAnswersItsTarget) {
  const auto docs = render_docs(report_inputs());
  ASSERT_TRUE(docs.has_value()) << docs.error();
  EXPECT_TRUE(has(docs->headline,
                  "| SVI-6: fit error at most 4 bp of forward | 0.25 bp of F on s1 | "
                  "[linux-zen1](results/linux-zen1/r1.txt), Linux | met, by 3.75 bp of F |"))
      << docs->headline;
  EXPECT_TRUE(has(docs->results_md,
                  "| `error` of row `s1` in report section `fit_error` of the latest result on "
                  "the host |"))
      << docs->results_md;
  EXPECT_TRUE(has(docs->headline, "`0123456` for AAD-1, AAD-5, MC-1, MEM-4 and SVI-6;"))
      << docs->headline;
  EXPECT_FALSE(has(docs->headline, "NOTICE")) << docs->headline;
}

TEST(DocsReport, ResultsMdRendersTheReportUnderItsHost) {
  const auto docs = render_docs(report_inputs());
  ASSERT_TRUE(docs.has_value()) << docs.error();
  const std::string& r = docs->results_md;
  const auto benchmarks = r.find("\n### Benchmarks\n");
  const auto reports = r.find("\n### Reports\n\n#### Fit error per slice\n");
  ASSERT_NE(benchmarks, std::string::npos) << r;
  ASSERT_NE(reports, std::string::npos) << r;
  EXPECT_LT(benchmarks, reports);
  EXPECT_TRUE(has(r, "| [r1.txt](results/linux-zen1/r1.txt) | result | `0123456` |")) << r;
  EXPECT_TRUE(has(r, "| row | error (bp_of_F) | quotes (count) | class |\n")) << r;
  EXPECT_TRUE(has(r, "commit `0123456789ab`, claim `result`")) << r;
  // The report file is not a benchmark: the benchmark table has the one row it had.
  EXPECT_EQ(r.find("| harness_selftest |", r.find("| harness_selftest |") + 1), std::string::npos);
}

TEST(DocsReport, NothingChangesWithoutAReportFile) {
  const DocsInputs plain = inputs();
  const auto before = render_docs(plain);
  ASSERT_TRUE(before.has_value()) << before.error();
  EXPECT_FALSE(has(before->results_md, "### Reports"));
  EXPECT_FALSE(has(before->headline, "the attribution is in")) << before->headline;
}

TEST(DocsReport, TheLatestRunAnswersAndTiesAreRefused) {
  DocsInputs in = report_inputs();
  in.files.push_back({.host_dir = "linux-zen1",
                      .name = "r0.txt",
                      .text = report_text("2026-01-02T03:04:05Z", "9")});
  in.files.push_back({.host_dir = "linux-zen1",
                      .name = "r2.txt",
                      .text = report_text("2026-01-02T03:04:08Z", "1.5")});
  const auto docs = render_docs(in);
  ASSERT_TRUE(docs.has_value()) << docs.error();
  EXPECT_TRUE(has(docs->headline, "| 1.5 bp of F on s1 | [linux-zen1](results/linux-zen1/r2.txt)"))
      << docs->headline;

  // A tie between earlier runs, a1 and r0, read before the latest, does not stand in its way.
  DocsInputs earlier = in;
  earlier.files.push_back({.host_dir = "linux-zen1",
                           .name = "a1.txt",
                           .text = report_text("2026-01-02T03:04:05Z", "7")});
  const auto past_tie = render_docs(earlier);
  ASSERT_TRUE(past_tie.has_value()) << past_tie.error();
  EXPECT_TRUE(
      has(past_tie->headline, "| 1.5 bp of F on s1 | [linux-zen1](results/linux-zen1/r2.txt)"))
      << past_tie->headline;

  in.files.push_back({.host_dir = "linux-zen1",
                      .name = "r3.txt",
                      .text = report_text("2026-01-02T03:04:08Z", "2")});
  const auto tie = render_docs(in);
  ASSERT_FALSE(tie.has_value());
  EXPECT_TRUE(has(tie.error(), "two runs of report section fit_error started at")) << tie.error();
}

TEST(DocsReport, OnlyAResultAnswers) {
  DocsInputs in = report_inputs();
  in.files.back().text = report_text("2026-01-02T03:04:06Z", "0.25", false, "none");
  const auto docs = render_docs(in);
  ASSERT_FALSE(docs.has_value());
  EXPECT_TRUE(has(docs.error(), "no result on this host has report section fit_error"))
      << docs.error();
}

TEST(DocsReport, RefusesACellThatIsMissingNotANumberOrNotForTheTarget) {
  const std::string head = "report_cell,SVI-6,linux-zen1,fit_error,";
  const std::string tail = ",4,at most 4 bp,at_most,1,bp of F,\n";
  for (const auto& [detail, error] :
       std::initializer_list<std::pair<std::string_view, std::string_view>>{
           {"s9/error", "has no cell s9/error"},
           {"s1/width", "has no cell s1/width"},
           {"s1/class", "is not a number"},
       }) {
    std::string row = head;
    row += detail;
    row += tail;
    const auto docs = render_docs(report_inputs(row));
    ASSERT_FALSE(docs.has_value()) << detail;
    EXPECT_TRUE(has(docs.error(), error)) << docs.error();
  }
  DocsInputs other =
      report_inputs("report_cell,SVI-7,linux-zen1,fit_error,s1/error,15,15 ms,at_most,1,ms,\n");
  other.benchmarks += "\n### SVI-7: calibration time: 15 ms per slice\n";
  const auto docs = render_docs(other);
  ASSERT_FALSE(docs.has_value());
  EXPECT_TRUE(has(docs.error(), "report section fit_error's targets do not name SVI-7"))
      << docs.error();
  const auto wrong_text = render_docs(report_inputs(
      "report_cell,SVI-6,linux-zen1,fit_error,s1/error,4,at most 3 bp,at_most,1,bp of F,\n"));
  ASSERT_FALSE(wrong_text.has_value());
  EXPECT_TRUE(has(wrong_text.error(), "the heading of SVI-6 does not say at most 3 bp"))
      << wrong_text.error();
}

TEST(DocsReport, AnIntegerCellAnswersAndScaleDivides) {
  const auto docs = render_docs(report_inputs(
      "report_cell,SVI-6,linux-zen1,fit_error,s2W4/quotes,40,at most 4 bp,at_least,10,,\n",
      "SVI-6,linux-zen1,,missed,files,results/linux-zen1/r1.txt,fit_error s2W4/quotes,,,\n",
      "missed"));
  ASSERT_TRUE(docs.has_value()) << docs.error();
  EXPECT_TRUE(has(docs->headline,
                  "| 5.1 | [linux-zen1](results/linux-zen1/r1.txt), Linux | "
                  "missed, by 34.90 |"))
      << docs->headline;
  EXPECT_TRUE(has(docs->results_md, ", divided by 10 |")) << docs->results_md;
}

TEST(DocsReport, ARowFromTheChainPointsToTheAttribution) {
  DocsInputs in = report_inputs();
  in.files.back().text = report_text("2026-01-02T03:04:06Z", "0.25", true);
  const auto docs = render_docs(in);
  ASSERT_TRUE(docs.has_value()) << docs.error();
  EXPECT_TRUE(has(docs->headline, "the attribution is in [NOTICE](NOTICE)")) << docs->headline;
  EXPECT_TRUE(has(docs->results_md, "    A synthetic attribution.\n")) << docs->results_md;
}

TEST(DocsReport, RefusesAReportFileOutOfTheGrammar) {
  DocsInputs in = report_inputs();
  std::string& text = in.files.back().text;
  text.replace(text.find("row s2W4"), 8, "row s1");
  const auto docs = render_docs(in);
  ASSERT_FALSE(docs.has_value());
  EXPECT_TRUE(has(docs.error(), "linux-zen1/r1.txt: ")) << docs.error();
  EXPECT_TRUE(has(docs.error(), "repeated row key s1")) << docs.error();
}

TEST(HeadlineSpec, RefusesAReportCellWithoutRowAndColumn) {
  const std::string header(kHeadlineSpecHeader);
  for (const std::string_view detail : {"s1", "s1/", "/error", "s1/error/x", ""}) {
    EXPECT_FALSE(parse_headline_spec(header + "\nreport_cell,SVI-6,linux-zen1,fit_error," +
                                     std::string(detail) + ",4,4 bp,at_most,1,,\n")
                     .has_value())
        << detail;
  }
  EXPECT_TRUE(parse_headline_spec(header + "\nreport_cell,SVI-6,linux-zen1,fit_error,s1/error,4,"
                                           "4 bp,at_most,1,,\n")
                  .has_value());
  EXPECT_FALSE(parse_headline_spec(header + "\nreport_cell,SVI-6,linux-zen1,Fit,s1/error,4,"
                                            "4 bp,at_most,1,,\n")
                   .has_value());
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
  // A build only after a workload, and only for a workload's row.
  for (const std::string_view source : {"w@", "@baseline", "w@base@line", "w@Base"}) {
    EXPECT_FALSE(parse_headline_spec(header + "\nmultiple,AAD-1,linux-zen1," + std::string(source) +
                                     ",tape/price,3,3,below,1,,\n")
                     .has_value())
        << source;
  }
  EXPECT_TRUE(parse_headline_spec(
                  header + "\nmultiple,AAD-1,linux-zen1,w@baseline,tape/price,3,3,below,1,,\n")
                  .has_value());
  EXPECT_FALSE(parse_headline_spec(header +
                                   "\nlargest,MEM-4,linux-zen1,tape_allocated_bytes@baseline,,50,"
                                   "below 50 MB,below,1e6,MB,\n")
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
  EXPECT_EQ(run->commit, "89abcde");
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

// --- results/verdicts.csv ---

// inputs() with AAD-1 met on its scalar reading and missed on lanes, a host-free test verdict
// (RNG-3), and a target measured with no verdict (AAD-8).
DocsInputs judged_inputs() {
  DocsInputs in = inputs();
  in.benchmarks = kBenchmarks;
  in.benchmarks +=
      "\n### AAD-8: second order\n\n### RNG-3: accuracy\n- Measured: missed, by a test.\n";
  state(in.benchmarks, "AAD-1", "on `linux-zen1`: 2, met; on lanes it misses.");
  state(in.benchmarks, "AAD-5", "on `linux-zen1`: missed.");
  state(in.benchmarks, "MC-1", "on `linux-zen1`: missed.");
  state(in.benchmarks, "MEM-4", "on `linux-zen1`: met.");
  state(in.benchmarks, "AAD-8", "on `linux-zen1`: reported, no bar; the parameters are fixed.");
  in.verdicts.replace(in.verdicts.find("AAD-1,linux-zen1,,met,"), 22,
                      "AAD-1,linux-zen1,scalar,met,");
  in.verdicts +=
      "AAD-1,linux-zen1,lanes,missed,files,results/linux-zen1/a.txt,,,,\n"
      "AAD-8,linux-zen1,,none,files,results/linux-zen1/a.txt,,,,\n"
      "RNG-3,any,,missed,test,,,,,the test\n";
  return in;
}

TEST(Verdicts, TheCountsNameEveryTargetAndTheTableEveryRow) {
  const auto docs = render_docs(judged_inputs());
  ASSERT_TRUE(docs.has_value()) << docs.error();
  const std::string& h = docs->headline;
  EXPECT_TRUE(has(h, "a target with a missed row is never counted as met. A row is not counted,"))
      << h;
  EXPECT_TRUE(has(h,
                  "- `linux-zen1`, from the files of commit `0123456`: 4 targets judged. Met, 1: "
                  "[MEM-4](BENCHMARKS.md#mem-4-per-path-tape-below-50-mb-in-the-worst-case). "
                  "Missed, 2: [AAD-5](BENCHMARKS.md#aad-5-within-125-times-a-hand-written-adjoint) "
                  "and [MC-1](BENCHMARKS.md#mc-1-asian-1044-million-pathss-single-thread). Met in "
                  "part, 1: [AAD-1](BENCHMARKS.md#aad-1-barrier-multiple-below-30). Not counted, "
                  "0. Measured with no verdict stated: [AAD-8](BENCHMARKS.md#aad-8-second-order)."
                  "\n"))
      << h;
  EXPECT_TRUE(has(h,
                  "- By tests whose \"Measured\" line names no host: 1 target judged. Met, 0. "
                  "Missed, 1: [RNG-3](BENCHMARKS.md#rng-3-accuracy). Met in part, 0. Not counted, "
                  "0.\n"))
      << h;
  EXPECT_TRUE(has(h, "[RESULTS.md](RESULTS.md#verdicts)")) << h;
  const std::string& r = docs->results_md;
  EXPECT_TRUE(has(r, "\n## Verdicts\n\n")) << r;
  EXPECT_TRUE(
      has(r,
          "| [AAD-1](BENCHMARKS.md#aad-1-barrier-multiple-below-30) | linux-zen1 | scalar | "
          "met | - | `w tape/price` | [a.txt](results/linux-zen1/a.txt) | a note, with a "
          "comma |\n"))
      << r;
  EXPECT_TRUE(has(r,
                  "| [RNG-3](BENCHMARKS.md#rng-3-accuracy) | any | - | missed | - | - | a test "
                  "| the test |\n"))
      << r;
  EXPECT_TRUE(has(r, "| linux-zen1 | - | none stated | - |")) << r;
}

TEST(Verdicts, RefusesAStatedVerdictLeftOut) {
  DocsInputs in = judged_inputs();
  in.verdicts.erase(in.verdicts.find("AAD-1,linux-zen1,lanes"));
  in.verdicts +=
      "AAD-8,linux-zen1,,none,files,results/linux-zen1/a.txt,,,,\n"
      "RNG-3,any,,missed,test,,,,,the test\n";
  const auto missed = render_docs(in);
  ASSERT_FALSE(missed.has_value());
  EXPECT_TRUE(has(missed.error(),
                  "AAD-1 on linux-zen1 is stated missed, and results/verdicts.csv "
                  "has no missed row for it"))
      << missed.error();

  in = judged_inputs();
  in.verdicts.erase(in.verdicts.find("RNG-3,any"));
  const auto any = render_docs(in);
  ASSERT_FALSE(any.has_value());
  EXPECT_TRUE(has(any.error(), "RNG-3 on any is stated missed")) << any.error();

  in = judged_inputs();
  state(in.benchmarks, "MEM-4", "on `mac-m4max`: met.");
  const auto host = render_docs(in);
  ASSERT_FALSE(host.has_value());
  EXPECT_TRUE(has(host.error(), "MEM-4 on mac-m4max is stated met")) << host.error();

  in = judged_inputs();
  in.verdicts.erase(in.verdicts.find("AAD-8,"),
                    in.verdicts.find("RNG-3,") - in.verdicts.find("AAD-8,"));
  const auto none = render_docs(in);
  ASSERT_FALSE(none.has_value());
  EXPECT_TRUE(has(none.error(), "AAD-8 on linux-zen1 is measured with no verdict")) << none.error();
}

TEST(Verdicts, RefusesARowNoLineStates) {
  DocsInputs in = judged_inputs();
  in.verdicts += "AAD-1,linux-zen1,other,none,files,results/linux-zen1/a.txt,,,,\n";
  const auto invented = render_docs(in);
  ASSERT_FALSE(invented.has_value());
  EXPECT_TRUE(has(invented.error(), "AAD-1 on linux-zen1 is none, which no \"Measured\" line"))
      << invented.error();

  in = judged_inputs();
  in.verdicts += "AAD-5,linux-zen1,skip,met,files,results/linux-zen1/a.txt,,,,\n";
  in.verdicts.replace(in.verdicts.find("AAD-5,linux-zen1,,"), 18, "AAD-5,linux-zen1,tape,");
  const auto met = render_docs(in);
  ASSERT_FALSE(met.has_value());
  EXPECT_TRUE(has(met.error(), "AAD-5 on linux-zen1 is met, which no \"Measured\" line"))
      << met.error();

  in = judged_inputs();
  in.verdicts += "MEM-4,mac-m4max,,met,test,,,,,\n";
  const auto host = render_docs(in);
  ASSERT_FALSE(host.has_value());
  EXPECT_TRUE(has(host.error(), "MEM-4 on mac-m4max has no \"Measured\" line")) << host.error();
}

TEST(Verdicts, AHeadlineRowIsNamedOnceAndAgrees) {
  DocsInputs in = judged_inputs();
  in.verdicts.replace(in.verdicts.find("MEM-4,linux-zen1,,met,"), 22, "MEM-4,linux-zen1,a,missed,");
  in.verdicts += "MEM-4,linux-zen1,b,met,files,results/linux-zen1/a.txt,,,,\n";
  state(in.benchmarks, "MEM-4", "on `linux-zen1`: missed.");
  const auto disagrees = render_docs(in);
  ASSERT_FALSE(disagrees.has_value());
  EXPECT_TRUE(has(disagrees.error(),
                  "headline MEM-4 on linux-zen1 (tape_allocated_bytes) computes "
                  "met, and results/verdicts.csv says missed"))
      << disagrees.error();

  in = judged_inputs();
  in.verdicts.replace(in.verdicts.find(",w price,"), 9, ",,");
  const auto unnamed = render_docs(in);
  ASSERT_FALSE(unnamed.has_value());
  EXPECT_TRUE(has(unnamed.error(), "headline MC-1 on linux-zen1 (w price) is named by 0 rows"))
      << unnamed.error();

  in = judged_inputs();
  in.verdicts.replace(in.verdicts.find(",w price,"), 9, ",w paths,");
  const auto unknown = render_docs(in);
  ASSERT_FALSE(unknown.has_value());
  EXPECT_TRUE(has(unknown.error(),
                  "names headline row w paths, which results/headline.csv does "
                  "not have") ||
              has(unknown.error(), "is named by 0 rows"))
      << unknown.error();
}

TEST(Verdicts, RefusesACitedFileThatIsNotInItsHostsResults) {
  DocsInputs in = judged_inputs();
  in.verdicts += "MEM-4,linux-zen1,other,met,files,results/linux-zen1/zz.txt,,,,\n";
  in.verdicts.replace(in.verdicts.find("MEM-4,linux-zen1,,"), 18, "MEM-4,linux-zen1,one,");
  const auto absent = render_docs(in);
  ASSERT_FALSE(absent.has_value());
  EXPECT_TRUE(has(absent.error(),
                  "results/linux-zen1/zz.txt is not a results file in "
                  "results/linux-zen1/"))
      << absent.error();

  in = judged_inputs();
  in.files.push_back(
      {.host_dir = "mac-m4max", .name = "m.txt", .text = results_text("mac-m4max", section())});
  in.verdicts += "AAD-8,mac-m4max,,none,files,results/mac-m4max/m.txt,,,,\n";
  in.verdicts.replace(in.verdicts.find("AAD-8,linux-zen1,,none,files,results/linux-zen1/a.txt"), 53,
                      "AAD-8,linux-zen1,,none,files,results/mac-m4max/m.txt");
  const auto other_host = render_docs(in);
  ASSERT_FALSE(other_host.has_value());
  EXPECT_TRUE(has(other_host.error(), "results/mac-m4max/m.txt is not results/linux-zen1/"))
      << other_host.error();
}

TEST(Verdicts, RefusesWhatIsNotAVerdictsFile) {
  const std::string header = std::string(kVerdictsHeader) + "\n";
  const std::string row = "AAD-1,linux-zen1,,met,files,results/linux-zen1/a.txt,,,,";
  EXPECT_TRUE(parse_verdicts(header + row + "\n").has_value());
  EXPECT_EQ(parse_verdicts(header + row + "a, b, c\n")->front().note, "a, b, c");
  for (const std::string_view bad : {
           "AAD-1,linux-zen1,,met,files,results/linux-zen1/a.txt,,",       // no run fields
           "aad-1,linux-zen1,,met,files,results/linux-zen1/a.txt,,,,",     // not an id
           "AAD-1,linux,,met,files,results/linux-zen1/a.txt,,,,",          // not a host
           "AAD-1,linux-zen1,,good,files,results/linux-zen1/a.txt,,,,",    // not a verdict
           "AAD-1,linux-zen1,,met,file,results/linux-zen1/a.txt,,,,",      // not a basis
           "AAD-1,linux-zen1,,met,files,,,,,",                             // files with none
           "AAD-1,linux-zen1,,met,test,results/linux-zen1/a.txt,,,,",      // a test with files
           "AAD-1,any,,met,files,results/any/a.txt,,,,",                   // any host on files
           "AAD-1,linux-zen1,,met,files,results/linux-zen1/../a.txt,,,,",  // a path out
           "AAD-1,linux-zen1,,met,files,results/linux-zen1/a.txt,a b c,,,",
           "AAD-1,linux-zen1,,met,files,results/linux-zen1/a.txt,,,,a | b",
       }) {
    EXPECT_FALSE(parse_verdicts(header + std::string(bad) + "\n").has_value()) << bad;
  }
  EXPECT_FALSE(parse_verdicts("target,host\n" + row + "\n").has_value());
  EXPECT_FALSE(parse_verdicts(header).has_value());
  const std::string two = "AAD-1,linux-zen1,lanes,met,files,results/linux-zen1/a.txt,,,,\n";
  EXPECT_TRUE(parse_verdicts(header + two + "AAD-1,linux-zen1,scalar,met,test,,,,,\n").has_value());
  EXPECT_TRUE(parse_verdicts(header + two + "AAD-1,mac-m4max,lanes,met,test,,,,,\n").has_value());
  const auto same = parse_verdicts(header + two + two);
  ASSERT_FALSE(same.has_value());
  EXPECT_TRUE(has(same.error(), "two rows of one condition")) << same.error();
  EXPECT_FALSE(parse_verdicts(header + two + row + "\n").has_value());  // empty beside another
  EXPECT_FALSE(parse_verdicts(header + row + "\n" + two).has_value());  // and in the other order
}

// inputs() with a split verdict (AAD-8 on lanes: the withdrawn run met, the rerun missed) and a
// marked one (RNG-4), each with its "Measured" line.
DocsInputs uncounted_inputs() {
  DocsInputs in = judged_inputs();
  in.benchmarks += "\n### RNG-4: per kernel\n";
  state(in.benchmarks, "AAD-8",
        "on `linux-zen1`, lanes: 1.9 in the withdrawn run, met; 2.2 in its rerun, missed; not "
        "counted.");
  state(in.benchmarks, "RNG-4", "on `linux-zen1`: 3.1 ns, missed, marked; not counted.");
  in.verdicts.replace(in.verdicts.find("AAD-8,linux-zen1,,none,"), 23,
                      "AAD-8,linux-zen1,reported,none,");
  in.verdicts +=
      "AAD-8,linux-zen1,lanes,split,files,results/linux-zen1/a.txt,,met 1.9,missed 2.2,a rerun\n"
      "RNG-4,linux-zen1,,marked,files,results/linux-zen1/a.txt,,,missed 3.1 ns,twice\n";
  return in;
}

TEST(Verdicts, ARowNotCountedGivesBothRunsAndCountsNeither) {
  const auto docs = render_docs(uncounted_inputs());
  ASSERT_TRUE(docs.has_value()) << docs.error();
  const std::string& h = docs->headline;
  EXPECT_TRUE(has(h, "such a row is listed as not counted and names no headline row. ")) << h;
  EXPECT_TRUE(
      has(h,
          "- `linux-zen1`, from the files of commit `0123456`: 4 targets judged. Met, 1: "
          "[MEM-4](BENCHMARKS.md#mem-4-per-path-tape-below-50-mb-in-the-worst-case). "
          "Missed, 2: [AAD-5](BENCHMARKS.md#aad-5-within-125-times-a-hand-written-adjoint) "
          "and [MC-1](BENCHMARKS.md#mc-1-asian-1044-million-pathss-single-thread). Met in "
          "part, 1: [AAD-1](BENCHMARKS.md#aad-1-barrier-multiple-below-30). Not counted, 2: "
          "[AAD-8](BENCHMARKS.md#aad-8-second-order) (lanes: the withdrawn run met at 1.9, "
          "the rerun missed at 2.2) and [RNG-4](BENCHMARKS.md#rng-4-per-kernel) (marked, "
          "failed the rule on every run, its last run missed at 3.1 ns). Measured with no "
          "verdict stated: [AAD-8](BENCHMARKS.md#aad-8-second-order) (reported).\n"))
      << h;
  const std::string& r = docs->results_md;
  EXPECT_TRUE(has(r,
                  "| [AAD-8](BENCHMARKS.md#aad-8-second-order) | linux-zen1 | lanes | not counted: "
                  "the runs differ | withdrawn run met at 1.9; rerun missed at 2.2 | - | "
                  "[a.txt](results/linux-zen1/a.txt) | a rerun |\n"))
      << r;
  EXPECT_TRUE(
      has(r,
          "| [RNG-4](BENCHMARKS.md#rng-4-per-kernel) | linux-zen1 | - | not counted: marked "
          "| last run missed at 3.1 ns | - | [a.txt](results/linux-zen1/a.txt) | twice |\n"))
      << r;
}

TEST(Verdicts, RefusesANotCountedVerdictLeftOutInventedOrHalfGiven) {
  DocsInputs in = uncounted_inputs();
  in.verdicts.erase(in.verdicts.find("RNG-4,"));
  const auto left_out = render_docs(in);
  ASSERT_FALSE(left_out.has_value());
  EXPECT_TRUE(has(left_out.error(),
                  "RNG-4 on linux-zen1 is stated not counted, and "
                  "results/verdicts.csv has no split or marked row for it"))
      << left_out.error();

  in = uncounted_inputs();
  const std::size_t split_line = in.benchmarks.find("- Measured on `linux-zen1`, lanes");
  in.benchmarks.erase(split_line, in.benchmarks.find('\n', split_line) + 1 - split_line);
  const auto invented = render_docs(in);
  ASSERT_FALSE(invented.has_value());
  EXPECT_TRUE(has(invented.error(), "AAD-8 on linux-zen1 is split, which no \"Measured\" line"))
      << invented.error();

  const std::string header = std::string(kVerdictsHeader) + "\n";
  const std::string head = "AAD-1,linux-zen1,,";
  const std::string files = ",files,results/linux-zen1/a.txt,";
  EXPECT_TRUE(parse_verdicts(header + head + "met" + files + ",met 1.9,met 2.0,\n").has_value());
  EXPECT_TRUE(
      parse_verdicts(header + head + "split" + files + ",met 1.9,missed 2.2,\n").has_value());
  EXPECT_TRUE(
      parse_verdicts(header + head + "marked" + files + ",met 1.9,missed 2.2,\n").has_value());
  EXPECT_TRUE(parse_verdicts(header + head + "marked" + files + ",,missed 2.2,\n").has_value());
  for (const std::string_view bad : {
           "split,,met 1.9,met 2.0,",           // a split whose runs agree
           "split,,,missed 2.2,",               // a split with one run
           "met,,met 1.9,missed 2.2,",          // runs that differ, counted
           "met,,missed 1.9,met 2.2,",          // the withdrawn run missed, counted met
           "missed,,met 1.9,met 2.2,",          // runs that are not the verdict
           "met,,met 1.9,,",                    // one run on a counted row
           "none,,met 1.9,met 2.0,",            // runs on a none row
           "marked,,met 1.9,,",                 // marked with no figure it publishes
           "split,w tape,met 1.9,missed 2.2,",  // a headline line for a verdict not counted
           "split,,fine 1.9,missed 2.2,",       // not an outcome
           "split,,met,missed 2.2,",            // no figure
       }) {
    std::string text = header;
    text += head;
    text += bad.substr(0, bad.find(','));
    text += files;
    text += bad.substr(bad.find(',') + 1);
    text += '\n';
    EXPECT_FALSE(parse_verdicts(text).has_value()) << text;
  }
}

// judged_inputs() with RNG-4 waiting for a run on linux-zen1: its "Waiting" line and its row.
DocsInputs waiting_inputs() {
  DocsInputs in = judged_inputs();
  in.benchmarks += "\n### RNG-4: per kernel\n";
  const std::size_t heading = in.benchmarks.find("### RNG-4:");
  in.benchmarks.insert(in.benchmarks.find('\n', heading) + 1,
                       "- Waiting on `linux-zen1`: its run was withdrawn, missed nothing yet.\n");
  in.verdicts += "RNG-4,linux-zen1,cycles,waiting,pending,,,,,runs again\n";
  return in;
}

TEST(Verdicts, AWaitingRowIsListedAfterTheCountsAndCountsNeither) {
  const auto docs = render_docs(waiting_inputs());
  ASSERT_TRUE(docs.has_value()) << docs.error();
  const std::string& h = docs->headline;
  EXPECT_TRUE(has(h,
                  "has a waiting row there, neither judged nor counted, listed after the "
                  "counts with what waits.\n\n"))
      << h;
  EXPECT_TRUE(has(h,
                  "- `linux-zen1`, from the files of commit `0123456`: 4 targets judged. Met, 1: "
                  "[MEM-4](BENCHMARKS.md#mem-4-per-path-tape-below-50-mb-in-the-worst-case). "
                  "Missed, 2: [AAD-5](BENCHMARKS.md#aad-5-within-125-times-a-hand-written-adjoint) "
                  "and [MC-1](BENCHMARKS.md#mc-1-asian-1044-million-pathss-single-thread). Met in "
                  "part, 1: [AAD-1](BENCHMARKS.md#aad-1-barrier-multiple-below-30). Not counted, "
                  "0. Measured with no verdict stated: [AAD-8](BENCHMARKS.md#aad-8-second-order). "
                  "Waiting for a run, neither judged nor counted: "
                  "[RNG-4](BENCHMARKS.md#rng-4-per-kernel) (cycles).\n"))
      << h;
  EXPECT_TRUE(has(docs->results_md,
                  "| [RNG-4](BENCHMARKS.md#rng-4-per-kernel) | linux-zen1 | cycles | waiting for a "
                  "run | - | - | none yet | runs again |\n"))
      << docs->results_md;
}

TEST(Verdicts, RefusesAWaitingRowLeftOutInventedOrWithAFigure) {
  DocsInputs in = waiting_inputs();
  in.verdicts.erase(in.verdicts.find("RNG-4,"));
  const auto left_out = render_docs(in);
  ASSERT_FALSE(left_out.has_value());
  EXPECT_TRUE(has(left_out.error(),
                  "RNG-4 on linux-zen1 is stated waiting, and results/verdicts.csv has no "
                  "waiting row for it"))
      << left_out.error();

  in = waiting_inputs();
  const std::size_t line = in.benchmarks.find("- Waiting on");
  in.benchmarks.replace(line, std::string_view("- Waiting").size(), "- Measured");
  const auto measured = render_docs(in);
  ASSERT_FALSE(measured.has_value());
  EXPECT_TRUE(has(measured.error(), "RNG-4 on linux-zen1 is stated missed") ||
              has(measured.error(), "RNG-4 on linux-zen1 is waiting, which no \"Measured\" line"))
      << measured.error();

  in = waiting_inputs();
  in.benchmarks.erase(line, in.benchmarks.find('\n', line) + 1 - line);
  const auto invented = render_docs(in);
  ASSERT_FALSE(invented.has_value());
  EXPECT_TRUE(has(invented.error(), "RNG-4 on linux-zen1 has no \"Measured\" line"))
      << invented.error();

  const std::string header = std::string(kVerdictsHeader) + "\n";
  EXPECT_TRUE(parse_verdicts(header + "RNG-4,mac-m4max,cycles,waiting,pending,,,,,\n").has_value());
  for (const std::string_view bad : {
           "RNG-4,mac-m4max,,waiting,files,results/mac-m4max/a.txt,,,,",    // a waiting file
           "RNG-4,mac-m4max,,waiting,pending,results/mac-m4max/a.txt,,,,",  // pending with files
           "RNG-4,mac-m4max,,waiting,test,,,,,",                            // not pending
           "RNG-4,mac-m4max,,met,pending,,,,,",                             // pending, counted
           "RNG-4,mac-m4max,,waiting,pending,,,met 1.9,,",                  // a run that waits
           "RNG-4,mac-m4max,,waiting,pending,,,,missed 2.2,",               // and its rerun
           "RNG-4,mac-m4max,,waiting,pending,,w tape,,,",                   // a headline row
           "RNG-4,any,,waiting,pending,,,,,",                               // on any host
       }) {
    EXPECT_FALSE(parse_verdicts(header + std::string(bad) + "\n").has_value()) << bad;
  }
}

TEST(Verdicts, MeasuredLinesGiveTheTargetHostsVerdictsAndAnchor) {
  const std::string text =
      "## 1. Cost\n\n### AAD-1: barrier, tape: multiple below 3.0\n\n"
      "- Source: a paper that met no one.\n"
      "- Measured on `linux-zen1`: 2.18. Met; the scalar tape misses.\n"
      "- Measured on `mac-m4max` and `linux-zen1`, by a test of the parameters.\n"
      "- Measured: met, by a test.\n"
      "## 14. Code-quality bar\n\n**Benchmarks**\n"
      "- B34. One adversarial workload.\n"
      "  - Measured on `linux-zen1`: published.\n"
      "- B35. Benchmarks in CI.\n"
      "**Other**\n"
      "- Measured on `linux-zen1`: met, outside any target.\n"
      "### MC-3: threads\n"
      "- Measured on `linux-zen1`: withdrawn run met, rerun missed; Not Counted.\n"
      "- Waiting on `mac-m4max`: withdrawn; it met once, not counted.\n"
      "- Wait on `mac-m4max`: no such line.\n";
  const std::vector<MeasuredLine> lines = measured_lines(text);
  ASSERT_EQ(lines.size(), 6U);
  EXPECT_TRUE(lines[5].waiting);  // its words state no verdict
  EXPECT_FALSE(lines[5].met || lines[5].missed || lines[5].not_counted);
  EXPECT_EQ(lines[5].hosts, std::vector<std::string>{"mac-m4max"});
  EXPECT_FALSE(lines[4].waiting);
  EXPECT_TRUE(lines[4].not_counted);
  EXPECT_FALSE(lines[4].met);  // the outcomes of its runs, not verdicts
  EXPECT_FALSE(lines[4].missed);
  EXPECT_FALSE(lines[0].not_counted);
  EXPECT_EQ(lines[0].line, 6U);
  EXPECT_EQ(lines[0].target, "AAD-1");
  EXPECT_EQ(lines[0].anchor, "aad-1-barrier-tape-multiple-below-30");
  EXPECT_EQ(lines[0].hosts, std::vector<std::string>{"linux-zen1"});
  EXPECT_TRUE(lines[0].met);
  EXPECT_TRUE(lines[0].missed);
  EXPECT_EQ(lines[1].hosts, (std::vector<std::string>{"linux-zen1", "mac-m4max"}));
  EXPECT_FALSE(lines[1].met);  // "parameters" holds "met" inside a word
  EXPECT_FALSE(lines[1].missed);
  EXPECT_TRUE(lines[2].hosts.empty());
  EXPECT_TRUE(lines[2].met);
  EXPECT_EQ(lines[3].target, "B34");
  EXPECT_EQ(lines[3].anchor, "14-code-quality-bar");
  EXPECT_FALSE(lines[3].met);
}

// "not met" states the other outcome, or none, and the word reader takes the word after "not";
// so a verdict word just after "not" or "never" is refused, and the line must say met or missed.
TEST(Verdicts, RefusesANegatedVerdictWord) {
  for (const std::string_view words : {"not met", "Not met", "never met", "not missed",
                                       "never missing", "not, met", "NOT MEETING"}) {
    DocsInputs in = judged_inputs();
    state(in.benchmarks, "MEM-4", "on `linux-zen1`: 1.05, " + std::string(words) + ".");
    const auto docs = render_docs(in);
    ASSERT_FALSE(docs.has_value()) << words;
    EXPECT_TRUE(has(docs.error(), "a verdict word just after \"not\" or \"never\""))
        << docs.error();
  }
  // A word that only ends in "not" negates nothing, and a negation before another word is left.
  for (const std::string_view words : {"met; knot met", "met, not by much", "met, never twice"}) {
    DocsInputs in = judged_inputs();
    state(in.benchmarks, "MEM-4", "on `linux-zen1`: 1.05, " + std::string(words) + ".");
    const auto docs = render_docs(in);
    EXPECT_TRUE(docs.has_value()) << words << ": " << docs.error();
  }
}

// The pages check catches a weakened word reader only through the words the pages use today; this
// holds both readers in ctest, the layer a developer runs first (docs/NOTES.md, "The verdict
// readers and the fault runner's tests in ctest").
TEST(Verdicts, AVerdictWordIsOneOfItsFormsAndNoWordThatBeginsWithIt) {
  const auto read = [](std::string_view word) {
    const std::vector<MeasuredLine> lines = measured_lines(
        "### AAD-1: x\n- Measured on `linux-zen1`: 2.18, " + std::string(word) + ".\n");
    EXPECT_EQ(lines.size(), 1U) << word;
    return lines.empty() ? std::pair{false, false} : std::pair{lines[0].met, lines[0].missed};
  };
  for (const std::string_view word : {"met", "Met", "MET", "meets", "meeting"}) {
    EXPECT_EQ(read(word), std::pair(true, false)) << word;
  }
  for (const std::string_view word : {"missed", "Missed", "misses", "missing"}) {
    EXPECT_EQ(read(word), std::pair(false, true)) << word;
  }
  for (const std::string_view word :
       {"metal", "method", "metric", "metre", "mets", "meted", "metadata", "meet", "unmet", "miss",
        "mission", "missile", "missive", "dismissed", "missedx", "metx"}) {
    EXPECT_EQ(read(word), std::pair(false, false)) << word;
  }

  // A row's verdict is one of its six words exactly.
  const std::string header = std::string(kVerdictsHeader) + "\n";
  const auto row = [&](std::string_view verdict, std::string_view rest) {
    return header + "RNG-4,mac-m4max,," + std::string(verdict) + "," + std::string(rest) + "\n";
  };
  constexpr std::string_view kFiles = "files,results/mac-m4max/a.txt,,,,";
  EXPECT_TRUE(parse_verdicts(row("met", kFiles)).has_value());
  EXPECT_TRUE(parse_verdicts(row("missed", kFiles)).has_value());
  EXPECT_TRUE(parse_verdicts(row("none", kFiles)).has_value());
  EXPECT_TRUE(parse_verdicts(row("waiting", "pending,,,,,")).has_value());
  EXPECT_TRUE(parse_verdicts(row("split", "files,results/mac-m4max/a.txt,,met 1.9,missed 2.2,"))
                  .has_value());
  EXPECT_TRUE(
      parse_verdicts(row("marked", "files,results/mac-m4max/a.txt,,,missed 2.2,")).has_value());
  for (const std::string_view bad :
       {"met in part",    "Met",     "MET",    "mets",       "metal",   "meets",
        "missed in part", "Missed",  "miss",   "misses",     "missing", "none stated",
        "None",           "nothing", "wait",   "waited",     "Waiting", "splits",
        "Split",          "mark",    "Marked", "met missed", ""}) {
    EXPECT_FALSE(parse_verdicts(row(bad, kFiles)).has_value()) << bad;
  }
  for (const std::string_view bad : {"waiting for a run", "waitingx"}) {
    EXPECT_FALSE(parse_verdicts(row(bad, "pending,,,,,")).has_value()) << bad;
  }
}

}  // namespace
}  // namespace aadp::results
