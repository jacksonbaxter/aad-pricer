// The report kind of results file: the same magic line and run header as a benchmark file
// (results/results_file.hpp), with report sections in place of a benchmark run. A tool that is
// not a Google Benchmark binary, such as the SVI fit, writes one, so that its figures can be
// stamped, filed under results/<host>/ and rendered into RESULTS.md.
//
//   @section report_svi_fit_error
//   type table
//   title Raw SVI fit error per slice
//   targets SVI-6,SVI-11
//   timing no
//   uses_chain yes
//   column svi6 real bp_of_F
//   column quotes integer count
//   column mm_class text none
//   row 202411 0.28719839201234567 65 type-1
//   @section attribution
//   <the chain's attribution, verbatim>
//
// A file is a benchmark file (a benchmark_json section, no report section) or a report file
// (report sections, no benchmark_json, nothing but report sections and the attribution); both
// or neither is refused. The lines of a report section come in the order above, each once, then
// one or more `column` lines and one or more `row` lines, fields separated by single spaces:
// - `type` is `table`, the only type there is.
// - `title` is free text in a safe character set; `targets` names the BENCHMARKS.md ids the
//   section answers, or `none`; `timing` says whether its values are timings; `uses_chain`
//   whether they are derived from the option chain, in which case the file must carry the
//   chain's attribution in an `attribution` section.
// - A column is a name, a kind and a unit (`none` for a pure number). A row is a key, unique in
//   the section, and one cell per column. A real cell is the shortest %g text that reads back as
//   the same finite double, so the text round-trips exactly and has one spelling; an integer is
//   plain decimal; a text cell is a short token in a safe character set.
// Everything here is a refusal rule with a named problem; no reader throws or reads out of
// bounds on bad input.
#ifndef AADP_RESULTS_REPORT_HPP_
#define AADP_RESULTS_REPORT_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "results/results_file.hpp"

namespace aadp::results {

inline constexpr std::string_view kReportPrefix = "report_";
inline constexpr std::string_view kAttributionSection = "attribution";

// A report file is far smaller than this; a benchmark file keeps the parser's own limit.
inline constexpr std::size_t kMaxReportFileBytes = std::size_t{1024} * 1024U;
inline constexpr std::size_t kMaxReportSections = 64;
inline constexpr std::size_t kMaxReportColumns = 32;
inline constexpr std::size_t kMaxReportRows = 1024;

// What the attribution section must say when a section is derived from the chain: the pinned
// lines of src/data/attribution.cpp (tests/data/attribution_test.cpp pins the same ones). The
// results area cannot link the data area, so these are the check, and the emitter writes the
// whole text through data/attribution.hpp.
inline constexpr auto kAttributionFragments = std::to_array<std::string_view>({
    "data.gov.tw dataset 11320, trade date 2024-11-15",
    "data.gov.tw dataset 11319, trade date 2024-11-15",
    "Open Government Data License: https://data.gov.tw/license",
    "Taiwan Futures Exchange (TAIFEX)",
    "not endorsed by",
});

enum class FileKind : std::uint8_t { kBenchmark, kReport };

enum class ReportProblem : std::uint8_t {
  kKind,            // both kinds, or neither
  kTooLarge,        // over kMaxReportFileBytes, or too many sections, columns or rows
  kUnknownSection,  // a section in a report file that is not a report section or the attribution
  kUnknownType,     // a report section whose type is not `table`
  kMalformed,       // a line out of order, missing, repeated or unreadable
  kBadColumn,       // a bad or repeated column name, kind or unit
  kBadRow,          // a bad or repeated row key, or the wrong number of cells
  kBadCell,         // a cell its column's kind does not admit, a non-finite number included
  kNoAttribution,   // a section derived from the chain, and no full attribution in the file
};

struct ReportError {
  ReportProblem problem;
  std::string detail;
};

enum class CellKind : std::uint8_t { kReal, kInteger, kText };

struct ReportColumn {
  std::string name;
  CellKind kind = CellKind::kReal;
  std::string unit = "none";
  bool operator==(const ReportColumn&) const = default;
};

struct ReportRow {
  std::string key;
  std::vector<std::string> cells;  // As written: real_cell, integer_cell or a text token
  bool operator==(const ReportRow&) const = default;
};

struct ReportTable {
  std::string name;  // The section's name after kReportPrefix
  std::string title;
  std::vector<std::string> targets;  // Empty: `targets none`
  bool timing = false;
  bool uses_chain = false;
  std::vector<ReportColumn> columns;
  std::vector<ReportRow> rows;
  bool operator==(const ReportTable&) const = default;

  [[nodiscard]] std::optional<std::size_t> column(std::string_view wanted) const;
  [[nodiscard]] const ReportRow* row(std::string_view key) const;
};

// Benchmark or report, from the sections alone.
std::expected<FileKind, ReportError> file_kind(const ResultsFile& file);

// The one spelling of a finite double: the shortest "%.<p>g" that reads back bit for bit. Empty
// for a non-finite value, which no cell admits.
std::string real_cell(double value);
std::string integer_cell(std::int64_t value);
// A real cell's value, if `text` is one: finite, and spelled as real_cell spells it.
std::optional<double> read_real_cell(std::string_view text);
std::optional<std::int64_t> read_integer_cell(std::string_view text);
// Either kind of number, as a double, for the headline.
std::optional<double> read_number_cell(const ReportColumn& column, std::string_view text);

// The section a table is written as. Refuses anything parse_report_section would refuse.
std::expected<ResultsSection, ReportError> write_report_section(const ReportTable& table);
std::expected<ReportTable, ReportError> parse_report_section(const ResultsSection& section);

// Every rule of a report file: its kind, its written size, its sections, each table and the
// attribution. The tables in file order.
std::expected<std::vector<ReportTable>, ReportError> read_report(const ResultsFile& file);

}  // namespace aadp::results

#endif  // AADP_RESULTS_REPORT_HPP_
