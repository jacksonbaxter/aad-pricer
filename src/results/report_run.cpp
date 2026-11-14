#include "results/report_run.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "platform/affinity.hpp"
#include "platform/cpu_info.hpp"
#include "platform/fp_state.hpp"
#include "platform/memory_info.hpp"
#include "platform/perf_counters.hpp"
#include "results/report.hpp"
#include "results/results_file.hpp"
#include "results/run_header.hpp"

namespace aadp::results {

std::expected<ReportRun, RunRefusal> ReportRun::begin(const RunOptions& options) {
  auto start = start_run(options);
  if (!start) {
    return std::unexpected(start.error());
  }
  auto counters = std::make_unique<platform::PerfCounterSession>();
  counters->start();
  return ReportRun(std::move(*start), std::move(counters));
}

std::expected<std::string, RunRefusal> ReportRun::finish(
    const ReportShape& shape, std::span<const ReportTable> tables,
    std::optional<std::string_view> attribution) {
  const platform::PerfCounterReport perf = counters_->stop();
  const auto fp_after = platform::check_fp_state();
  if (!fp_after) {
    return std::unexpected(
        RunRefusal{.exit_code = kRunExitFpState,
                   .message = "FP state changed during the run: " + fp_after.error()});
  }
  const auto refused = [](std::string message) {
    return std::unexpected(RunRefusal{.exit_code = kRunExitFailure, .message = std::move(message)});
  };

  ResultsFile file;
  // The attribution first, before anything the chain shaped.
  if (attribution) {
    std::string body(*attribution);
    if (!body.empty() && !body.ends_with('\n')) {
      body += '\n';
    }
    file.sections.push_back({.name = std::string(kAttributionSection), .body = std::move(body)});
  }
  for (const ReportTable& table : tables) {
    auto section = write_report_section(table);
    if (!section) {
      return refused("report " + table.name + ": " + section.error().detail);
    }
    file.sections.push_back(std::move(*section));
  }

  // Read last, as the harness does: the CPU and frequency the run finished on.
  const platform::AffinityState affinity = platform::read_affinity();
  file.header = run_header_fields(start_, shape.run, *fp_after, affinity);
  file.header.insert(file.header.end(), shape.header.begin(), shape.header.end());
  add_run_late_fields(file.header, platform::read_frequency_info(affinity.current.value_or(0)),
                      perf, platform::peak_rss_bytes());

  for (const auto keys : {std::span<const std::string_view>(kRunHeaderKeys),
                          std::span<const std::string_view>(kPerfHeaderKeys)}) {
    if (const auto missing = missing_keys(file, keys); !missing.empty()) {
      return refused("header is missing " + std::string(missing.front()));
    }
  }
  if (auto report = read_report(file); !report) {
    return refused(report.error().detail);
  }
  auto text = write_results_file(file);
  if (!text) {
    return refused(text.error());
  }
  return std::move(*text);
}

}  // namespace aadp::results
