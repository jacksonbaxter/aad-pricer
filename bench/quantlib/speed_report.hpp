// QL-1's speed figures (docs/NOTES.md, "Stage 10") as the text line the bench prints and as a
// report table (results/report.hpp), both from one record per timed call, so the two cannot say
// different things. No QuantLib here, so every build compiles and tests it.
#ifndef AADP_BENCH_QUANTLIB_SPEED_REPORT_HPP_
#define AADP_BENCH_QUANTLIB_SPEED_REPORT_HPP_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "quantlib/compare.hpp"
#include "results/report.hpp"

namespace aadp::qlcompare {

// One timed estimator: a row of the protocol (R1, R2.K100, R9b, R12) and a column (ql-default,
// ql-tuned-nolto, ql-tuned, ours-scalar, ours-lanes, ours-lanes-ours-one-step, ...).
struct SpeedRecord {
  std::string row;
  std::string column;
  Timing timing;
};

inline constexpr std::string_view kSpeedTextHeader =
    "speed,row,column,paths,steps,median_s,min_s,max_s,paths_per_s,ns_per_path_step";
inline constexpr std::string_view kSpeedTableName = "ql_speed";

// %.10g, as every number of the bench's text is printed.
std::string FormatNumber(double x);

// The text line under kSpeedTextHeader.
std::string SpeedLineText(const SpeedRecord& record);

// One row per record, keyed <row>.<column>, every cell the record's own double: the paths,
// steps, median, least and greatest time in s, paths/s, ns per path-step and the repetitions.
// Timed, targets QL-1 and QL-3, not derived from the option chain.
results::ReportTable SpeedTable(std::span<const SpeedRecord> records, std::int64_t repetitions);

// The registered paths of a row divided by `scale`, a power of two from 1 to kMaxSpeedScale, for
// tests and iteration runs; nullopt for any other scale or a count it would not divide into
// whole batches.
inline constexpr std::uint64_t kMaxSpeedScale = 64;
std::optional<std::uint64_t> ScaledPaths(std::uint64_t paths, std::uint64_t scale);

}  // namespace aadp::qlcompare

#endif  // AADP_BENCH_QUANTLIB_SPEED_REPORT_HPP_
