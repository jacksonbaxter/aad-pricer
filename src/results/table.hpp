// The markdown results table. It is the only way numbers reach the README. One table per host,
// because a Mac number and a Linux number must never sit side by side.
#ifndef AADP_RESULTS_TABLE_HPP_
#define AADP_RESULTS_TABLE_HPP_

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "results/results_file.hpp"

namespace aadp::results {

// One table. Refuses files from more than one host, and any file that is not reportable.
std::expected<std::string, std::string> render_host_table(std::span<const ResultsFile> files);

// One table per host, in host-label order.
std::expected<std::string, std::string> render_tables(std::span<const ResultsFile> files);

}  // namespace aadp::results

#endif  // AADP_RESULTS_TABLE_HPP_
