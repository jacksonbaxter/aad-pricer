// RESULTS.md and the README's headline table, generated from committed files and from nothing
// else. Stage 11 reruns the benchmarks and regenerates both, so no number lives in this code:
//
//   results/headline.csv   which result answers which target, the target's threshold, and the
//                          oracle runs RESULTS.md shows side by side
//   BENCHMARKS.md          each target's heading, quoted as the target's name
//   results/<host>/*.txt   the stamped results files and fault-injection reports
//   references/measured_results*.csv   the measured oracle runs the spec names
//
// The headline sits in README.md between kHeadlineBegin and kHeadlineEnd. `aadp_results_table
// --docs <root> --check` regenerates both and fails on any difference; scripts/check.sh runs it.
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

// One line of results/headline.csv. Kinds:
//   measured_run      an oracle run for RESULTS.md: source is its CSV under references/
//   multiple          detail "<variant>/<base>" of workload `source`: mean time over mean time
//   paths_per_second  detail "<variant>" of workload `source`: paths over mean time
//   largest           the largest value of section key `source` in any AAD section of the host
//   oracle_run        the run in `source`, which must be a measured_run: rows that pass
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
// A `run=N passed=N failed=N` comment must agree with the rows, and a `Host <label>,` comment
// names the host.
struct MeasuredRun {
  std::vector<std::string> comments;  // without the leading "# "
  std::vector<MeasuredRow> rows;
  std::string host;  // empty if no comment names it
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
  std::string benchmarks;
  std::vector<DocsFile> files;
  std::map<std::string, std::string, std::less<>> measured_runs;  // spec path -> text
};

struct Docs {
  std::string headline;    // the README block, ending in a newline
  std::string results_md;  // the whole of RESULTS.md
};

std::expected<Docs, std::string> render_docs(const DocsInputs& inputs);

// The first line, counted from 1, where two texts differ, or 0 if they are equal.
std::size_t first_difference_line(std::string_view a, std::string_view b);

}  // namespace aadp::results

#endif  // AADP_RESULTS_DOCS_HPP_
