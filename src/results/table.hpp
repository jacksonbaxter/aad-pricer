// The markdown results table. It is the only way numbers reach the README. One table per host,
// because a Mac number and a Linux number must never sit side by side.
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
  // For looking at a run while working on it; such a table never goes in the README.
  bool iteration = false;
};

// One table. Refuses files from more than one host, and any file that is not reportable unless
// options.iteration. Files with AAD sections get the AAD tables after it (results/aad_table.hpp).
std::expected<std::string, std::string> render_host_table(std::span<const ResultsFile> files,
                                                          const TableOptions& options = {});

// One table per host, in host-label order.
std::expected<std::string, std::string> render_tables(std::span<const ResultsFile> files,
                                                      const TableOptions& options = {});

// Google Benchmark's mean real time of `name` across repetitions, or the single run's.
std::optional<std::string> benchmark_mean_time(const JsonValue& root, std::string_view name);

}  // namespace aadp::results

#endif  // AADP_RESULTS_TABLE_HPP_
