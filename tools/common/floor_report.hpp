// A local-volatility run's floor report (pricer/local_vol_model.hpp), as the report tools write it:
// machine-readable lines for the text output and report tables (results/report.hpp) for --report,
// from the same tally, so a test can hold one against the other. Shared by the tools that price
// off a surface (aadp_essvi_report, aadp_local_vol_report).
//
//   floors,<key>,path_steps,paths,non_finite,non_positive_variance,calendar,butterfly,
//          below_floor,capped,paths_with_floor,paths_with_cap
//   range,<key>,<floored|capped>,<all|seg1|seg2|seg3>,count,k_min,k_max,t_min,t_max
//
// A range line and row are written only where the count is not zero: an empty range has no k or
// t, and the counts line already says it is empty.
#ifndef AADP_TOOLS_COMMON_FLOOR_REPORT_HPP_
#define AADP_TOOLS_COMMON_FLOOR_REPORT_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "pricer/local_vol_model.hpp"
#include "svi/local_vol.hpp"

#if AADP_REPORT_MODE
#include "results/report.hpp"
#endif

namespace aadp::tools {

// The floored and capped classes of svi::LocalVarianceClass, in the order of the floors line.
inline constexpr std::array<svi::LocalVarianceClass, 6> kFloorClasses = {
    svi::LocalVarianceClass::kNonFinite,  svi::LocalVarianceClass::kNonPositiveVariance,
    svi::LocalVarianceClass::kCalendar,   svi::LocalVarianceClass::kButterfly,
    svi::LocalVarianceClass::kBelowFloor, svi::LocalVarianceClass::kCapped};

inline const char* SegmentKey(std::size_t s) {
  constexpr std::array<const char*, 3> kKeys = {"seg1", "seg2", "seg3"};
  return s < kKeys.size() ? kKeys[s] : "seg";
}

// Each non-empty range: "all" first, then every segment up to `segments`.
template <typename Visit>
void ForEachRange(const pricer::FloorTally& t, std::size_t segments, Visit visit) {
  for (const bool capped : {false, true}) {
    const auto& ranges = capped ? t.capped : t.floored;
    const char* kind = capped ? "capped" : "floored";
    const pricer::FloorRange all = pricer::FloorTally::Total(ranges);
    if (all.count > 0) visit(kind, "all", all);
    for (std::size_t s = 0; s < segments && s < ranges.size(); ++s) {
      if (ranges[s].count > 0) visit(kind, SegmentKey(s), ranges[s]);
    }
  }
}

// Full precision, for comparing a report's cells with the text.
inline void PrintFloorLines(const std::string& key, const pricer::FloorTally& t,
                            std::size_t segments) {
  std::printf("floors,%s,%llu,%llu", key.c_str(), static_cast<unsigned long long>(t.path_steps),
              static_cast<unsigned long long>(t.paths));
  for (const svi::LocalVarianceClass c : kFloorClasses) {
    std::printf(",%llu", static_cast<unsigned long long>(t.by_class[static_cast<std::size_t>(c)]));
  }
  std::printf(",%llu,%llu\n", static_cast<unsigned long long>(t.paths_with_floor),
              static_cast<unsigned long long>(t.paths_with_cap));
  ForEachRange(
      t, segments, [&key](const char* kind, const char* segment, const pricer::FloorRange& r) {
        std::printf("range,%s,%s,%s,%llu,%.17g,%.17g,%.17g,%.17g\n", key.c_str(), kind, segment,
                    static_cast<unsigned long long>(r.count), r.k_min, r.k_max, r.t_min, r.t_max);
      });
}

#if AADP_REPORT_MODE
inline std::string CountCell(std::uint64_t n) {
  return results::integer_cell(static_cast<std::int64_t>(n));
}

inline std::vector<results::ReportColumn> FloorColumns() {
  using results::CellKind;
  std::vector<results::ReportColumn> columns = {
      {.name = "path_steps", .kind = CellKind::kInteger, .unit = "count"},
      {.name = "paths", .kind = CellKind::kInteger, .unit = "count"}};
  for (const svi::LocalVarianceClass c : kFloorClasses) {
    std::string name = svi::LocalVarianceClassName(c);
    for (char& ch : name) ch = ch == '-' ? '_' : ch;
    columns.push_back({.name = std::move(name), .kind = CellKind::kInteger, .unit = "count"});
  }
  columns.push_back({.name = "paths_with_floor", .kind = CellKind::kInteger, .unit = "count"});
  columns.push_back({.name = "paths_with_cap", .kind = CellKind::kInteger, .unit = "count"});
  return columns;
}

inline results::ReportRow FloorRow(std::string key, const pricer::FloorTally& t) {
  results::ReportRow row{.key = std::move(key), .cells = {}};
  row.cells.push_back(CountCell(t.path_steps));
  row.cells.push_back(CountCell(t.paths));
  for (const svi::LocalVarianceClass c : kFloorClasses) {
    row.cells.push_back(CountCell(t.by_class[static_cast<std::size_t>(c)]));
  }
  row.cells.push_back(CountCell(t.paths_with_floor));
  row.cells.push_back(CountCell(t.paths_with_cap));
  return row;
}

inline std::vector<results::ReportColumn> RangeColumns() {
  using results::CellKind;
  return {{.name = "count", .kind = CellKind::kInteger, .unit = "count"},
          {.name = "k_min", .kind = CellKind::kReal, .unit = "log_moneyness"},
          {.name = "k_max", .kind = CellKind::kReal, .unit = "log_moneyness"},
          {.name = "t_min", .kind = CellKind::kReal, .unit = "years"},
          {.name = "t_max", .kind = CellKind::kReal, .unit = "years"}};
}

// Rows <key>.<floored|capped>.<all|segN>, one per non-empty range.
inline void AddRangeRows(results::ReportTable& table, const std::string& key,
                         const pricer::FloorTally& t, std::size_t segments) {
  ForEachRange(
      t, segments, [&](const char* kind, const char* segment, const pricer::FloorRange& r) {
        table.rows.push_back(
            {.key = key + "." + kind + "." + segment,
             .cells = {CountCell(r.count), results::real_cell(r.k_min), results::real_cell(r.k_max),
                       results::real_cell(r.t_min), results::real_cell(r.t_max)}});
      });
}
#endif

}  // namespace aadp::tools

#endif  // AADP_TOOLS_COMMON_FLOOR_REPORT_HPP_
