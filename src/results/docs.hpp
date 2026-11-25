// RESULTS.md and the README's headline table, generated from committed files and from nothing
// else. Stage 11 reruns the benchmarks and regenerates both, so no number lives in this code:
//
//   results/headline.csv   which result answers which target, the target's threshold, and the
//                          oracle runs RESULTS.md shows side by side
//   results/verdicts.csv   every verdict BENCHMARKS.md's "Measured" lines state, one row each
//   BENCHMARKS.md          each target's heading, quoted as the target's name, and its
//                          "Measured" lines, which verdicts.csv must answer row for row
//   results/<host>/*.txt   the stamped results files, benchmark or report, and fault-injection
//                          reports
//   references/measured_results*.csv   the measured oracle runs the spec names
//
// The headline sits in README.md between kHeadlineBegin and kHeadlineEnd, with the count of
// verdicts by host under it. `aadp_results_table --docs <root> --check` regenerates both files
// and fails on any difference; scripts/check.sh runs it.
#ifndef AADP_RESULTS_DOCS_HPP_
#define AADP_RESULTS_DOCS_HPP_

#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aadp::results {

inline constexpr std::string_view kHeadlineBegin =
    "<!-- BEGIN GENERATED HEADLINE: aadp_results_table --docs . --write; do not edit -->";
inline constexpr std::string_view kHeadlineEnd = "<!-- END GENERATED HEADLINE -->";
inline constexpr std::string_view kHeadlineSpecHeader =
    "kind,id,host,source,detail,target,target_text,better,scale,unit,label";
inline constexpr std::string_view kMeasuredRunHeader = "id,estimate,reference,se,bound,verdict";
inline constexpr std::string_view kFaultReportMagic = "aadp-fault-injection 1";
inline constexpr std::string_view kVerdictsHeader =
    "target,host,condition,verdict,basis,files,headline,withdrawn,rerun,note";
// The host of a verdict by a test whose "Measured" line names no host.
inline constexpr std::string_view kAnyHost = "any";

// One line of results/headline.csv. Kinds:
//   measured_run      an oracle run for RESULTS.md: source is its CSV under references/
//   multiple          detail "<variant>/<base>" of workload `source`: mean time over mean time
//   paths_per_second  detail "<variant>" of workload `source`: paths over mean time
//                     For both, `source` may be "<workload>@<build>": only files whose
//                     simd_cmake_target is <build> answer, so "aad1-barrier@baseline" takes the
//                     release preset's run on a host that also ran release-avx2.
//   largest           the largest value of section key `source` in any AAD section of the host
//   oracle_run        the run in `source`, which must be a measured_run: rows that pass
//   report_cell       detail "<row key>/<column>" of report section `source` (results/report.hpp,
//                     the name after report_), a number column, in the latest result on the host
//                     by run_started_at; the section's targets must name the id
// Every kind but measured_run is a headline row, in file order. `better` is below, at_most or
// at_least; the target and the measured value are compared after dividing the value by `scale`.
struct HeadlineRow {
  std::string kind;
  std::string id;
  std::string host;
  std::string source;
  std::string detail;
  double target = 0.0;
  std::string target_text;  // must appear in the BENCHMARKS.md heading of `id`
  std::string better;
  double scale = 1.0;
  std::string unit;
  std::string label;
};

struct HeadlineSpec {
  std::vector<HeadlineRow> measured_runs;
  std::vector<HeadlineRow> rows;
};

std::expected<HeadlineSpec, std::string> parse_headline_spec(std::string_view text);

struct MeasuredRow {
  std::string id;
  std::string estimate;
  std::string reference;
  std::string se;
  std::string bound;
  std::string verdict;  // pass or FAIL
};

// references/measured_results*.csv: `#` comment lines, the header, then one row per comparison.
// A `run=N passed=N failed=N` comment must agree with the rows, a `Host <label>,` comment names
// the host, and a `run at commit <hash>` comment the commit the run ran at. An oracle_run headline
// row takes its commit from there, and is refused without one.
struct MeasuredRun {
  std::vector<std::string> comments;  // without the leading "# "
  std::vector<MeasuredRow> rows;
  std::string host;    // empty if no comment names it
  std::string commit;  // empty if no comment names it; else 7 to 40 hex digits
  std::size_t passed = 0;
};

std::expected<MeasuredRun, std::string> parse_measured_run(std::string_view text);

// scripts/fault_injection.py's report: the magic, `key: value` lines up to `columns:`, one line
// per fault, and a `summary:` line.
struct FaultReport {
  std::map<std::string, std::string, std::less<>> header;
  std::size_t faults = 0;
  std::string summary;
};

std::expected<FaultReport, std::string> parse_fault_report(std::string_view text);

// The text of BENCHMARKS.md's `### <id>: ...` heading, without the hashes, if there is one.
std::optional<std::string> target_heading(std::string_view benchmarks, std::string_view id);

// One line of results/verdicts.csv: one verdict that a "Measured" line of BENCHMARKS.md states.
//   target     the BENCHMARKS.md id: a `### <id>:` heading's, or a code-quality bar item's (B34)
//   host       a host label, or kAnyHost for a verdict by a test whose line names no host
//   condition  which reading, where the target has several on the host (one thread, 4 lanes, a
//              slice, the stretch); empty only for the target's one row on the host. A none
//              row's condition is printed beside the target in the README's counts, so what
//              must be read with an unjudged figure is there
//   verdict    met or missed, counted; none: measured, with no verdict stated; or one of two
//              verdicts that are NOT COUNTED, neither as met nor as missed:
//                split   a step withdrawn under a host's rule and run again gave one outcome in
//                        the withdrawn run and the other in the rerun
//                marked  the figure rests on a step that failed the rule on every run it was
//                        allowed, published with its mark
//   basis      files: the stamped files in `files`; test: no file, the note names the test
//   files      results/<host>/<name>, space-separated, each a file in results/<host>/
//   headline   empty, or the headline.csv row of the same target and host whose verdict this is:
//              "<source> <detail>", or "<source>" for a row with no detail. Empty on a row not
//              counted, which has no headline line
//   withdrawn  empty, or "<met|missed> <figure>": a withdrawn run's outcome and figure
//   rerun      empty, or "<met|missed> <figure>": the outcome and figure of the run that answers
//              (the rerun of a withdrawn step, or a marked step's last run)
//   note       the rest of the line, commas and all: what must travel with the verdict
// No field but the note holds a comma or a `|`. (target, host, condition) is unique. A split
// row gives both runs, with different outcomes; a marked row gives its rerun; a met or missed
// row gives both runs or neither, and both outcomes are its verdict; a none row gives neither.
//
// render_docs checks the rows against BENCHMARKS.md's "Measured" lines (measured_lines below):
// every target, host and verdict a line states has a row, and every row has a line that states
// it, so a judged target can be neither left out nor invented; a target measured on a host with
// no verdict stated has a `none` row there, and one whose line says "not counted" has a split or
// marked row. Every headline row but an oracle run is named by exactly one verdict row, whose
// verdict must be the headline's (met, missed, or none for an INVALID run). Files are refused
// unless they are under results/<host>/ of the row's own host.
struct VerdictRow {
  std::string target;
  std::string host;
  std::string condition;
  std::string verdict;
  std::string basis;
  std::vector<std::string> files;
  std::string headline;
  std::string withdrawn;
  std::string rerun;
  std::string note;
};

std::expected<std::vector<VerdictRow>, std::string> parse_verdicts(std::string_view text);

// A "Measured" line of BENCHMARKS.md: a bullet, at any depth, whose text begins "Measured", in a
// `### <id>:` target's section or under a top-level `- B<n>.` bar item. The hosts are the host
// labels it names in backticks; a line that names none is a host-free verdict by a test, if it
// states one. A verdict is a word: met, meets or meeting; missed, misses or missing. A line that
// says "not counted" states a verdict not counted, and its verdict words are the outcomes of its
// runs, not verdicts.
struct MeasuredLine {
  std::size_t line = 0;  // counted from 1
  std::string target;
  std::string anchor;  // GitHub's anchor of the heading above it; empty if it has a non-ASCII byte
  std::vector<std::string> hosts;
  bool met = false;
  bool missed = false;
  bool not_counted = false;
};

std::vector<MeasuredLine> measured_lines(std::string_view benchmarks);

// The README with the text between its two marker lines replaced by `block`. Refuses a README
// without exactly one of each marker, in order.
std::expected<std::string, std::string> splice_headline(std::string_view readme,
                                                        std::string_view block);

// A file under results/<host_dir>/: a stamped results file or a fault-injection report.
struct DocsFile {
  std::string host_dir;
  std::string name;
  std::string text;
};

struct DocsInputs {
  std::string spec;
  std::string verdicts;
  std::string benchmarks;
  std::vector<DocsFile> files;
  std::map<std::string, std::string, std::less<>> measured_runs;  // spec path -> text
};

struct Docs {
  std::string headline;    // the README block with the verdict counts, ending in a newline
  std::string results_md;  // the whole of RESULTS.md
};

std::expected<Docs, std::string> render_docs(const DocsInputs& inputs);

// The first line, counted from 1, where two texts differ, or 0 if they are equal.
std::size_t first_difference_line(std::string_view a, std::string_view b);

}  // namespace aadp::results

#endif  // AADP_RESULTS_DOCS_HPP_
