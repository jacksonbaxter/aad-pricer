#include "quantlib/speed_report.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>

#include "quantlib/compare.hpp"
#include "results/report.hpp"

namespace aadp::qlcompare {

std::string FormatNumber(double x) {
  std::array<char, 40> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.10g", x);
  return {buffer.data()};
}

std::string SpeedLineText(const SpeedRecord& record) {
  const Timing& t = record.timing;
  return "speed," + record.row + "," + record.column + "," + std::to_string(t.paths) + "," +
         std::to_string(t.steps) + "," + FormatNumber(t.median_s) + "," + FormatNumber(t.min_s) +
         "," + FormatNumber(t.max_s) + "," + FormatNumber(PathsPerSecond(t)) + "," +
         FormatNumber(NanosecondsPerPathStep(t));
}

results::ReportTable SpeedTable(std::span<const SpeedRecord> records, std::int64_t repetitions) {
  using results::CellKind;
  using results::integer_cell;
  using results::real_cell;
  results::ReportTable table{
      .name = std::string(kSpeedTableName),
      .title = "QuantLib against this project on one thread: one timed pricing of every path",
      .targets = {"QL-1", "QL-3"},
      .timing = true,
      .uses_chain = false,
      .columns = {{.name = "row", .kind = CellKind::kText, .unit = "none"},
                  {.name = "column", .kind = CellKind::kText, .unit = "none"},
                  {.name = "paths", .kind = CellKind::kInteger, .unit = "count"},
                  {.name = "steps", .kind = CellKind::kInteger, .unit = "count"},
                  {.name = "median", .kind = CellKind::kReal, .unit = "s"},
                  {.name = "min", .kind = CellKind::kReal, .unit = "s"},
                  {.name = "max", .kind = CellKind::kReal, .unit = "s"},
                  {.name = "paths_per_s", .kind = CellKind::kReal, .unit = "paths/s"},
                  {.name = "ns_per_path_step", .kind = CellKind::kReal, .unit = "ns"},
                  {.name = "repetitions", .kind = CellKind::kInteger, .unit = "count"}},
      .rows = {}};
  for (const SpeedRecord& r : records) {
    const Timing& t = r.timing;
    table.rows.push_back(
        {.key = r.row + "." + r.column,
         .cells = {r.row, r.column, integer_cell(static_cast<std::int64_t>(t.paths)),
                   integer_cell(t.steps), real_cell(t.median_s), real_cell(t.min_s),
                   real_cell(t.max_s), real_cell(PathsPerSecond(t)),
                   real_cell(NanosecondsPerPathStep(t)), integer_cell(repetitions)}});
  }
  return table;
}

std::optional<std::uint64_t> ScaledPaths(std::uint64_t paths, std::uint64_t scale) {
  if (scale == 0 || scale > kMaxSpeedScale || !std::has_single_bit(scale)) return std::nullopt;
  const std::uint64_t scaled = paths / scale;
  if (scaled * scale != paths || scaled == 0 || scaled % kBatches != 0) return std::nullopt;
  return scaled;
}

}  // namespace aadp::qlcompare
