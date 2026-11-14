// What a tool needs to write a report file (results/report.hpp) with the run header of
// results/run_header.hpp. An emitter is a tool's own main with three calls around its work:
//
//   RunOptions run;                                   // the run flags, from take_run_flags
//   auto session = ReportRun::begin(run);             // before the work: refusals, timer, counters
//   ... compute, then fill ReportTable values with real_cell / integer_cell ...
//   auto text = session->finish(shape, tables, attribution);  // header, sections, checks
//   std::cout << *text;
//
// finish() writes nothing a reader would refuse: it reads its own output back with read_report,
// so a table derived from the chain without the attribution, a non-finite cell or a repeated
// key fails in the emitter, not later in the stamp. scripts/run_bench.sh then runs the emitter
// through measure.sh and the stamp exactly as it runs a benchmark binary.
#ifndef AADP_RESULTS_REPORT_RUN_HPP_
#define AADP_RESULTS_REPORT_RUN_HPP_

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform/perf_counters.hpp"
#include "results/report.hpp"
#include "results/results_file.hpp"
#include "results/run_header.hpp"

namespace aadp::results {

struct ReportShape {
  RunShape run;                     // claim, repetitions, batches per repetition, warm-up
  std::vector<HeaderField> header;  // The emitter's own header fields, after run_started_at
};

class ReportRun {
 public:
  // start_run, then the counters, which run until finish.
  static std::expected<ReportRun, RunRefusal> begin(const RunOptions& options);

  // The whole file's text. `attribution` becomes the attribution section; pass the chain's text
  // (data/attribution.hpp) whenever a table is derived from the chain.
  std::expected<std::string, RunRefusal> finish(const ReportShape& shape,
                                                std::span<const ReportTable> tables,
                                                std::optional<std::string_view> attribution);

  [[nodiscard]] const RunStart& start() const { return start_; }

 private:
  ReportRun(RunStart start, std::unique_ptr<platform::PerfCounterSession> counters)
      : start_(std::move(start)), counters_(std::move(counters)) {}

  RunStart start_;
  std::unique_ptr<platform::PerfCounterSession> counters_;
};

}  // namespace aadp::results

#endif  // AADP_RESULTS_REPORT_RUN_HPP_
