// The markdown results table. It and results/docs.hpp, which builds RESULTS.md and the README's
// headline from it, are the only way numbers reach the docs. One table per host, because a Mac
// number and a Linux number must never sit side by side.
#ifndef AADP_RESULTS_TABLE_HPP_
#define AADP_RESULTS_TABLE_HPP_

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "results/json_lite.hpp"
#include "results/results_file.hpp"

namespace aadp::results {

struct TableOptions {
  // Admit files that are not reportable, under a banner that says the numbers are iteration only.
  // For looking at a run while working on it; such a table never goes in the README or RESULTS.md.
  bool iteration = false;
  // A `### <host>` line first. RESULTS.md puts the table under its own host heading.
  bool host_heading = true;
};

// One table. Refuses files from more than one host, and any file that is not reportable unless
// options.iteration. Files with AAD sections get the AAD tables after it (results/aad_table.hpp),
// and report files their tables after those (render_report_tables), with no benchmark table if
// every file is a report file.
std::expected<std::string, std::string> render_host_table(std::span<const ResultsFile> files,
                                                          const TableOptions& options = {});

// The report sections of report files (results/report.hpp), each a table under `#### <title>`
// with its commit, claim, targets and units, then the option chain's attribution, once for each
// text the files carry, if a section is derived from the chain. Admits files as above.
std::expected<std::string, std::string> render_report_tables(std::span<const ResultsFile> files,
                                                             const TableOptions& options = {});

// One table per host, in host-label order.
std::expected<std::string, std::string> render_tables(std::span<const ResultsFile> files,
                                                      const TableOptions& options = {});

// Google Benchmark's mean real time of `name` across repetitions, or the single run's.
std::optional<std::string> benchmark_mean_time(const JsonValue& root, std::string_view name);

}  // namespace aadp::results

#endif  // AADP_RESULTS_TABLE_HPP_
