// A report emitter that measures nothing of this project: two small tables of fixed values and
// one timing of a fixed floating-point loop. It exercises results/report_run.hpp end to end for
// the stamp, the table and scripts/run_bench.sh, and is the smallest example of an emitter. Its
// numbers are not results of any kind, so it claims none.
//
//   aadp_report_selftest [run flags] [--repetitions=<n>]
//
// The run flags are results/run_header.hpp's: --source-root=, --reportable, --pin-cpu= and
// --meant-idle-cpus=. Everything goes to stdout as one raw results file.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

#include "results/report.hpp"
#include "results/report_run.hpp"
#include "results/run_header.hpp"

namespace {

using aadp::results::CellKind;
using aadp::results::integer_cell;
using aadp::results::real_cell;
using aadp::results::ReportTable;

constexpr std::int64_t kDefaultRepetitions = 5;
constexpr std::int64_t kMaxRepetitions = 1000;
constexpr int kLoopLength = 1 << 16;

std::optional<std::int64_t> repetitions_flag(std::string_view arg) {
  constexpr std::string_view kFlag = "--repetitions=";
  if (!arg.starts_with(kFlag)) {
    return std::nullopt;
  }
  const std::string text(arg.substr(kFlag.size()));
  std::int64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9' || value > kMaxRepetitions) {
      return std::int64_t{0};
    }
    value = (value * 10) + (c - '0');
  }
  return text.empty() || value > kMaxRepetitions ? 0 : value;
}

// A fixed loop, so the timing has something to time.
double loop() {
  double sum = 0.0;
  for (int i = 1; i <= kLoopLength; ++i) {
    sum = (sum * 0.5) + (1.0 / static_cast<double>(i));
  }
  return sum;
}

ReportTable values_table() {
  ReportTable table{.name = "selftest_values",
                    .title = "Fixed values: the grammar's three kinds of cell",
                    .targets = {},
                    .timing = false,
                    .uses_chain = false,
                    .columns = {{.name = "real", .kind = CellKind::kReal, .unit = "none"},
                                {.name = "count", .kind = CellKind::kInteger, .unit = "count"},
                                {.name = "label", .kind = CellKind::kText, .unit = "none"}},
                    .rows = {}};
  table.rows.push_back({.key = "one", .cells = {real_cell(1.0), integer_cell(1), "first"}});
  table.rows.push_back({.key = "third", .cells = {real_cell(1.0 / 3.0), integer_cell(3), "x+y"}});
  table.rows.push_back(
      {.key = "small", .cells = {real_cell(-2.5e-300), integer_cell(-7), "type-1"}});
  return table;
}

ReportTable timing_table(const std::vector<double>& ms, double sum) {
  std::vector<double> sorted = ms;
  std::ranges::sort(sorted);
  const std::size_t n = sorted.size();
  const double median = n % 2 == 1 ? sorted[n / 2] : 0.5 * (sorted[(n / 2) - 1] + sorted[n / 2]);
  return {.name = "selftest_timing",
          .title = "A fixed loop, timed: not a result",
          .targets = {},
          .timing = true,
          .uses_chain = false,
          .columns = {{.name = "median", .kind = CellKind::kReal, .unit = "ms"},
                      {.name = "min", .kind = CellKind::kReal, .unit = "ms"},
                      {.name = "repetitions", .kind = CellKind::kInteger, .unit = "count"},
                      {.name = "checksum", .kind = CellKind::kReal, .unit = "none"}},
          .rows = {{.key = "loop",
                    .cells = {real_cell(median), real_cell(sorted.front()),
                              integer_cell(static_cast<std::int64_t>(n)), real_cell(sum)}}}};
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  aadp::results::RunOptions run;
  auto rest = aadp::results::take_run_flags(argc, argv, run);
  if (!rest) {
    std::fprintf(stderr, "report_selftest: %s\n", rest.error().c_str());
    return aadp::results::kRunExitUsage;
  }
  std::int64_t repetitions = kDefaultRepetitions;
  for (const std::string& arg : *rest) {
    const auto value = repetitions_flag(arg);
    if (!value || *value < 1) {
      std::fprintf(stderr, "report_selftest: bad option %s\n", arg.c_str());
      return aadp::results::kRunExitUsage;
    }
    repetitions = *value;
  }

  auto session = aadp::results::ReportRun::begin(run);
  if (!session) {
    std::fprintf(stderr, "report_selftest: %s\n", session.error().message.c_str());
    return session.error().exit_code;
  }
  using Clock = std::chrono::steady_clock;
  std::vector<double> ms;
  double sum = 0.0;
  for (std::int64_t rep = 0; rep < repetitions; ++rep) {
    const auto t0 = Clock::now();
    sum = loop();
    const auto t1 = Clock::now();
    ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  const std::vector<ReportTable> tables = {values_table(), timing_table(ms, sum)};
  const aadp::results::ReportShape shape{.run = {.claim = "none",
                                                 .repetitions = repetitions,
                                                 .batches_per_repetition = 1,
                                                 .warmup = "none"},
                                         .header = {}};
  auto text = session->finish(shape, tables, std::nullopt);
  if (!text) {
    std::fprintf(stderr, "report_selftest: %s\n", text.error().message.c_str());
    return text.error().exit_code;
  }
  std::cout << *text;
  std::cout.flush();
  return std::cout ? 0 : aadp::results::kRunExitFailure;
} catch (const std::exception& error) {
  std::fprintf(stderr, "report_selftest: %s\n", error.what());
  return aadp::results::kRunExitFailure;
}
