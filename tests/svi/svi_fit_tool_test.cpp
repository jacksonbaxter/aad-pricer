// Runs the real aadp_svi_fit binary on synthetic files made from hand-written SVI parameters.
// Nothing here comes from a TAIFEX file.

#include <gtest/gtest.h>
#include <sys/wait.h>  // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../data/taifex_fixture.hpp"
#include "data/attribution.hpp"
#include "data/taifex_options.hpp"
#include "synthetic_chain.hpp"
#if AADP_SVI_FIT_REPORT
#include "results/report.hpp"
#include "results/results_file.hpp"
#endif

namespace aadp {
namespace {

namespace fs = std::filesystem;

struct RunResult {
  std::string output;
  int status = 0;
};

std::string ShellArg(const std::string& text) { return "'" + text + "'"; }

RunResult RunTool(const std::string& arguments, bool with_stderr = true) {
  const std::string command =
      ShellArg(AADP_SVI_FIT_PATH) + " " + arguments + (with_stderr ? " 2>&1" : " 2>/dev/null");
  FILE* pipe = popen(command.c_str(), "r");
  EXPECT_NE(pipe, nullptr);
  RunResult result{.output = "", .status = -1};
  if (pipe == nullptr) return result;
  std::array<char, 4096> buffer{};
  std::size_t count = 0;
  while ((count = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
    result.output.append(buffer.data(), count);
  }
  result.status = pclose(pipe);
  return result;
}

void WriteFile(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(out.good()) << path;
}

std::string Price(double value) {
  std::array<char, 32> text{};
  std::snprintf(text.data(), text.size(), "%.1f", value);
  return text.data();
}

std::string OptionsCsv(const std::vector<taifex::OptionRow>& rows) {
  std::string csv = testing::kOptionsHeader + "\r\n";
  for (const taifex::OptionRow& row : rows) {
    testing::OptionFields f = testing::option_fields();
    f[testing::kOptDate] = std::to_string(row.trade_date);
    f[testing::kOptContract] = row.contract;
    f[testing::kOptExpiry] = row.expiry;
    f[testing::kOptStrike] = std::to_string(static_cast<long>(row.strike));
    f[testing::kOptRight] =
        std::string(row.right == taifex::OptionRight::kCall ? testing::kCall : testing::kPut);
    const std::string p = Price(row.settlement_price.value_or(0.0));
    f[testing::kOptOpen] = p;
    f[testing::kOptHigh] = p;
    f[testing::kOptLow] = p;
    f[testing::kOptLast] = p;
    f[testing::kOptVolume] = "1";
    f[testing::kOptSettlement] = p;
    f[testing::kOptOpenInterest] = "1";
    f[testing::kOptBid] = "-";
    f[testing::kOptAsk] = "-";
    f[testing::kOptHistHigh] = "-";
    f[testing::kOptHistLow] = "-";
    csv += testing::join(f) + "\r\n";
  }
  return csv;
}

std::string FuturesCsv() {
  testing::FuturesFields f = testing::futures_fields();
  f[testing::kFutDate] = "20241115";
  f[testing::kFutContract] = "TX";
  f[testing::kFutExpiry] = "202411";
  f[testing::kFutSettlement] = "22800";
  return testing::kFuturesHeader + "\r\n" + testing::join(f) + "\r\n";
}

class SviFitTool : public ::testing::Test {
 protected:
  void SetUp() override {
    // ctest runs each test as its own process, in parallel, so no two can share these files.
    dir_ = fs::path(AADP_SVI_FIT_TEST_SCRATCH) /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::create_directories(dir_);
    WriteFile(dir_ / "options.csv",
              OptionsCsv(testing::SyntheticRows(Surface(), 20000.0, 25000.0, 100.0)));
    WriteFile(dir_ / "futures.csv", FuturesCsv());
  }

  // Hand-written SVI parameters, arbitrage-free.
  [[nodiscard]] virtual std::vector<testing::SyntheticExpiry> Surface() const {
    return {{.expiry = "202411",
             .forward = 22800.0,
             .discount = 0.9999,
             .params = {.a = 1e-4, .b = 0.012, .rho = -0.45, .m = 0.002, .sigma = 0.025}},
            {.expiry = "202411W4",
             .forward = 22810.0,
             .discount = 0.9995,
             .params = {.a = 2e-4, .b = 0.02, .rho = -0.35, .m = 0.005, .sigma = 0.04}},
            {.expiry = "202412",
             .forward = 22830.0,
             .discount = 0.998,
             .params = {.a = 5e-4, .b = 0.04, .rho = -0.5, .m = 0.01, .sigma = 0.06}}};
  }

  [[nodiscard]] std::string Arguments() const {
    return "--options " + ShellArg((dir_ / "options.csv").string()) + " --futures " +
           ShellArg((dir_ / "futures.csv").string());
  }

  [[nodiscard]] const fs::path& Dir() const { return dir_; }

 private:
  fs::path dir_;
};

TEST_F(SviFitTool, ReportOpensWithTheFullAttribution) {
  const RunResult result = RunTool(Arguments());
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
}

TEST_F(SviFitTool, HeaderSaysSettlementsNotTradableQuotesAndThirtyThreeDays) {
  const RunResult result = RunTool(Arguments());
  EXPECT_NE(result.output.find("not tradable quotes"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("Maturity reaches 33 days only"), std::string::npos)
      << result.output;
}

TEST_F(SviFitTool, ReportsEverySliceTheCalendarAndNoTiming) {
  const RunResult result = RunTool(Arguments());
  EXPECT_EQ(result.status, 0) << result.output;
  for (const char* expected :
       {"== slice 202411 ==", "== slice 202411W4 ==", "== slice 202412 ==",
        "TX settlement       22800.00", "active bounds", "objective           Phi", "SVI-6",
        "SVI-11", "input butterflies", "model butterflies", "Martini-Mingone",
        "== calendar, adjacent fitted slices ==", "202411 -> 202411W4", "202411W4 -> 202412",
        "== machine-readable ==", "fit,202411,", "fit,202411W4,", "fit,202412,"}) {
    EXPECT_NE(result.output.find(expected), std::string::npos) << expected << "\n" << result.output;
  }
  EXPECT_EQ(result.output.find("timing"), std::string::npos) << result.output;
  EXPECT_EQ(result.output.find("FAILED"), std::string::npos) << result.output;
}

TEST_F(SviFitTool, TimingIsLabelledIterationOnly) {
  const RunResult result = RunTool(Arguments() + " --timing 1");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
  EXPECT_NE(result.output.find("ITERATION ONLY, not a result"), std::string::npos) << result.output;
}

TEST_F(SviFitTool, MissingFileFailsAndSaysHowToGetIt) {
  const RunResult result = RunTool("--options " + ShellArg((Dir() / "absent.csv").string()) +
                                   " --futures " + ShellArg((Dir() / "futures.csv").string()));
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.output.find("scripts/fetch_data.sh"), std::string::npos) << result.output;
}

TEST_F(SviFitTool, BadArgumentsAreAUsageError) {
  const RunResult result = RunTool("--timing 0");
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.output.find("usage: aadp_svi_fit"), std::string::npos) << result.output;
}

TEST_F(SviFitTool, TheMachineReadableLinesCoverSviNine) {
  const RunResult result = RunTool(Arguments());
  EXPECT_EQ(result.status, 0) << result.output;
  for (const char* expected : {"arbitrage,202411,", "arbitrage,202411W4,", "arbitrage,202412,",
                               "calendar,202411,202411W4,", "calendar,202411W4,202412,"}) {
    EXPECT_NE(result.output.find(expected), std::string::npos) << expected << "\n" << result.output;
  }
}

#if AADP_SVI_FIT_REPORT

// The comma-separated fields of every machine-readable line that starts with `kind,`, without
// the column header line.
std::vector<std::vector<std::string>> Lines(const std::string& output, std::string_view kind) {
  std::vector<std::vector<std::string>> out;
  std::size_t at = 0;
  while (at < output.size()) {
    const std::size_t end = std::min(output.find('\n', at), output.size());
    const std::string line = output.substr(at, end - at);
    at = end + 1;
    if (!line.starts_with(std::string(kind) + ",")) continue;
    std::vector<std::string> fields;
    std::size_t from = 0;
    while (true) {
      const std::size_t comma = line.find(',', from);
      fields.push_back(line.substr(from, comma == std::string::npos ? comma : comma - from));
      if (comma == std::string::npos) break;
      from = comma + 1;
    }
    if (fields.size() > 1 && (fields[1] == "expiry" || fields[1] == "earlier")) continue;
    out.push_back(std::move(fields));
  }
  return out;
}

double Full(const std::string& text) { return std::strtod(text.c_str(), nullptr); }

// The report's position cells: a real cell's spelling, or +inf, -inf or none.
std::string Position(const std::string& text) {
  const double k = Full(text);
  if (std::isnan(k)) return "none";
  if (std::isinf(k)) return k > 0.0 ? "+inf" : "-inf";
  return results::real_cell(k);
}

class SviFitReport : public SviFitTool {
 protected:
  void SetUp() override {
    SviFitTool::SetUp();
    text_ = RunTool(Arguments());
    report_ = RunTool("--report --timing 2 " + Arguments(), false);
    ASSERT_EQ(text_.status, 0) << text_.output;
    ASSERT_EQ(report_.status, 0) << report_.output;
    auto file = results::parse_results_file(report_.output);
    ASSERT_TRUE(file.has_value()) << file.error() << "\n" << report_.output;
    file_ = std::move(*file);
    auto tables = results::read_report(file_);
    ASSERT_TRUE(tables.has_value()) << tables.error().detail;
    tables_ = std::move(*tables);
  }

  [[nodiscard]] const results::ReportTable& Table(std::string_view name) const {
    for (const results::ReportTable& table : tables_) {
      if (table.name == name) return table;
    }
    ADD_FAILURE() << "no table " << name;
    return tables_.front();
  }

  // The cell of `row` in `column` of table `name`.
  [[nodiscard]] std::string Cell(std::string_view name, std::string_view row,
                                 std::string_view column) const {
    const results::ReportTable& table = Table(name);
    const results::ReportRow* entry = table.row(row);
    const auto index = table.column(column);
    if (entry == nullptr || !index) {
      ADD_FAILURE() << name << " has no cell " << row << "/" << column;
      return {};
    }
    return entry->cells[*index];
  }

  [[nodiscard]] double Real(std::string_view name, std::string_view row,
                            std::string_view column) const {
    const auto value = results::read_real_cell(Cell(name, row, column));
    EXPECT_TRUE(value.has_value()) << name << " " << row << "/" << column;
    return value.value_or(-1.0);
  }

  void ExpectArbitrageTablesHoldTheTextsDoubles() const;

  [[nodiscard]] const RunResult& Text() const { return text_; }
  [[nodiscard]] const RunResult& Report() const { return report_; }
  [[nodiscard]] const results::ResultsFile& File() const { return file_; }
  [[nodiscard]] const std::vector<results::ReportTable>& Tables() const { return tables_; }

 private:
  RunResult text_;
  RunResult report_;
  results::ResultsFile file_;
  std::vector<results::ReportTable> tables_;
};

TEST_F(SviFitReport, IsOneRawResultsFileOnStdoutWithTheRunHeader) {
  EXPECT_TRUE(Report().output.starts_with("aadp-results 1\n")) << Report().output;
  EXPECT_TRUE(results::missing_keys(File(), results::kRunHeaderKeys).empty());
  EXPECT_TRUE(results::missing_keys(File(), results::kPerfHeaderKeys).empty());
  EXPECT_EQ(File().value("claim"), "result");
  EXPECT_EQ(File().value("repetitions"), "2");
  EXPECT_EQ(results::file_kind(File()), results::FileKind::kReport);
  EXPECT_NE(File().required("report_inputs").find("not tradable quotes"), std::string_view::npos);
  EXPECT_NE(File().required("report_inputs").find("33 days only"), std::string_view::npos);
}

TEST_F(SviFitReport, CarriesTheWholeAttributionFirstAndEveryTableSaysItUsesTheChain) {
  ASSERT_FALSE(File().sections.empty());
  EXPECT_EQ(File().sections.front().name, "attribution");
  EXPECT_EQ(File().sections.front().body, std::string(attribution_text()));
  for (const results::ReportTable& table : Tables()) {
    EXPECT_TRUE(table.uses_chain) << table.name;
  }
}

TEST_F(SviFitReport, WritesExactlyTheSviSixNineElevenAndSevenTables) {
  std::vector<std::string> names;
  names.reserve(Tables().size());
  for (const results::ReportTable& table : Tables()) names.push_back(table.name);
  // A table without rows is not written, so the violations table is there only if there are any.
  std::vector<std::string> expected = {"svi_fit_error", "svi_fit_arbitrage", "svi_fit_calendar"};
  if (!Lines(Text().output, "violation").empty()) expected.emplace_back("svi_fit_violations");
  expected.emplace_back("svi_fit_time");
  expected.emplace_back("svi_fit_surface_time");
  EXPECT_EQ(names, expected);
  EXPECT_EQ(Table("svi_fit_error").targets, (std::vector<std::string>{"SVI-6", "SVI-11"}));
  EXPECT_EQ(Table("svi_fit_arbitrage").targets, (std::vector<std::string>{"SVI-9"}));
  EXPECT_EQ(Table("svi_fit_time").targets, (std::vector<std::string>{"SVI-7"}));
  EXPECT_EQ(Table("svi_fit_surface_time").targets, (std::vector<std::string>{"SVI-8"}));
  for (const results::ReportTable& table : Tables()) {
    EXPECT_EQ(table.timing, table.name.ends_with("_time")) << table.name;
  }
}

// Derived figures only: no column a settlement, a strike, the forward or the discount could be
// read from, and one row per slice or pair, never per strike.
TEST_F(SviFitReport, HoldsDerivedFiguresOnly) {
  std::vector<std::string> columns;
  for (const results::ReportTable& table : Tables()) {
    for (const results::ReportColumn& column : table.columns) {
      columns.push_back(table.name + "." + column.name);
    }
  }
  const bool any_violation = !Lines(Text().output, "violation").empty();
  std::erase_if(columns, [any_violation](const std::string& name) {
    return !any_violation && name.starts_with("svi_fit_violations.");
  });
  if (!any_violation) {
    columns.insert(columns.end() - 6, {"svi_fit_violations.kind", "svi_fit_violations.k_begin",
                                       "svi_fit_violations.k_end", "svi_fit_violations.k_worst",
                                       "svi_fit_violations.value", "svi_fit_violations.bound",
                                       "svi_fit_violations.points"});
  }
  EXPECT_EQ(columns, (std::vector<std::string>{"svi_fit_error.quotes",
                                               "svi_fit_error.svi6",
                                               "svi_fit_error.svi11",
                                               "svi_fit_error.objective_per_dof",
                                               "svi_fit_arbitrage.model_triples",
                                               "svi_fit_arbitrage.model_non_convex",
                                               "svi_fit_arbitrage.grid_points",
                                               "svi_fit_arbitrage.min_g",
                                               "svi_fit_arbitrage.k_min_g",
                                               "svi_fit_arbitrage.martini_mingone",
                                               "svi_fit_arbitrage.violations",
                                               "svi_fit_calendar.grid_points",
                                               "svi_fit_calendar.min_gap",
                                               "svi_fit_calendar.k_min_gap",
                                               "svi_fit_calendar.violations",
                                               "svi_fit_violations.kind",
                                               "svi_fit_violations.k_begin",
                                               "svi_fit_violations.k_end",
                                               "svi_fit_violations.k_worst",
                                               "svi_fit_violations.value",
                                               "svi_fit_violations.bound",
                                               "svi_fit_violations.points",
                                               "svi_fit_time.median",
                                               "svi_fit_time.min",
                                               "svi_fit_time.repetitions",
                                               "svi_fit_surface_time.median",
                                               "svi_fit_surface_time.min",
                                               "svi_fit_surface_time.repetitions"}));
  EXPECT_EQ(Table("svi_fit_error").rows.size(), 3U);
  EXPECT_EQ(Table("svi_fit_arbitrage").rows.size(), 3U);
  EXPECT_EQ(Table("svi_fit_calendar").rows.size(), 2U);
  EXPECT_EQ(Table("svi_fit_time").rows.size(), 3U);
  EXPECT_EQ(Table("svi_fit_surface_time").rows.size(), 1U);
  if (any_violation) {
    EXPECT_EQ(Table("svi_fit_violations").rows.size(), Lines(Text().output, "violation").size());
  }
}

TEST_F(SviFitReport, TheErrorTableHoldsTheTextsDoubles) {
  const auto fits = Lines(Text().output, "fit");
  ASSERT_EQ(fits.size(), 3U);
  for (const auto& f : fits) {
    ASSERT_GE(f.size(), 17U);  // the active bounds, last, may hold commas
    const std::string& expiry = f[1];
    const double n = Full(f[6]);
    EXPECT_EQ(Cell("svi_fit_error", expiry, "quotes"), f[6]);
    EXPECT_EQ(Real("svi_fit_error", expiry, "svi6"), Full(f[13])) << expiry;
    EXPECT_EQ(Real("svi_fit_error", expiry, "svi11"), Full(f[14])) << expiry;
    EXPECT_EQ(Real("svi_fit_error", expiry, "objective_per_dof"), Full(f[12]) / (n - 5.0))
        << expiry;
  }
}

void SviFitReport::ExpectArbitrageTablesHoldTheTextsDoubles() const {
  const auto slices = Lines(Text().output, "arbitrage");
  ASSERT_EQ(slices.size(), 3U);
  for (const auto& f : slices) {
    ASSERT_EQ(f.size(), 9U);
    const std::string& expiry = f[1];
    EXPECT_EQ(Cell("svi_fit_arbitrage", expiry, "model_triples"), f[2]);
    EXPECT_EQ(Cell("svi_fit_arbitrage", expiry, "model_non_convex"), f[3]);
    EXPECT_EQ(Cell("svi_fit_arbitrage", expiry, "grid_points"), f[4]);
    EXPECT_EQ(Real("svi_fit_arbitrage", expiry, "min_g"), Full(f[5])) << expiry;
    EXPECT_EQ(Real("svi_fit_arbitrage", expiry, "k_min_g"), Full(f[6])) << expiry;
    EXPECT_EQ(Cell("svi_fit_arbitrage", expiry, "martini_mingone"), f[7]);
    EXPECT_EQ(Cell("svi_fit_arbitrage", expiry, "violations"), f[8]);
  }
  const auto pairs = Lines(Text().output, "calendar");
  ASSERT_EQ(pairs.size(), 2U);
  for (const auto& f : pairs) {
    ASSERT_EQ(f.size(), 7U);
    const std::string key = f[1] + "-" + f[2];
    EXPECT_EQ(Cell("svi_fit_calendar", key, "grid_points"), f[3]);
    EXPECT_EQ(Real("svi_fit_calendar", key, "min_gap"), Full(f[4])) << key;
    EXPECT_EQ(Real("svi_fit_calendar", key, "k_min_gap"), Full(f[5])) << key;
    EXPECT_EQ(Cell("svi_fit_calendar", key, "violations"), f[6]);
  }
  for (const auto& f : Lines(Text().output, "violation")) {
    ASSERT_EQ(f.size(), 10U);
    const std::string key = f[1] + "." + f[2];
    EXPECT_EQ(Cell("svi_fit_violations", key, "kind"), f[3]);
    EXPECT_EQ(Cell("svi_fit_violations", key, "k_begin"), Position(f[4])) << key;
    EXPECT_EQ(Cell("svi_fit_violations", key, "k_end"), Position(f[5])) << key;
    EXPECT_EQ(Cell("svi_fit_violations", key, "k_worst"), Position(f[6])) << key;
    EXPECT_EQ(Real("svi_fit_violations", key, "value"), Full(f[7])) << key;
    EXPECT_EQ(Real("svi_fit_violations", key, "bound"), Full(f[8])) << key;
    EXPECT_EQ(Cell("svi_fit_violations", key, "points"), f[9]);
  }
}

TEST_F(SviFitReport, TheArbitrageTablesHoldTheTextsDoubles) {
  ExpectArbitrageTablesHoldTheTextsDoubles();
}

// 202411W4's total variance below 202411's near the money: a calendar crossing between them,
// unrepaired, so the violations table has rows.
class SviFitReportCrossing : public SviFitReport {
 protected:
  [[nodiscard]] std::vector<testing::SyntheticExpiry> Surface() const override {
    std::vector<testing::SyntheticExpiry> surface = SviFitReport::Surface();
    surface[0].params.a = 6e-4;
    surface[1].params.a = 0.0;
    return surface;
  }
};

TEST_F(SviFitReportCrossing, ListsEveryViolationWithTheTextsDoubles) {
  ASSERT_FALSE(Lines(Text().output, "violation").empty()) << Text().output;
  EXPECT_EQ(Table("svi_fit_violations").rows.size(), Lines(Text().output, "violation").size());
  EXPECT_NE(Cell("svi_fit_calendar", "202411-202411W4", "violations"), "0");
  ExpectArbitrageTablesHoldTheTextsDoubles();
}

TEST_F(SviFitReport, TheTimedSectionHasEverySliceAndItsRepetitions) {
  for (const std::string expiry : {"202411", "202411W4", "202412"}) {
    EXPECT_GT(Real("svi_fit_time", expiry, "median"), 0.0) << expiry;
    EXPECT_GT(Real("svi_fit_time", expiry, "min"), 0.0) << expiry;
    EXPECT_LE(Real("svi_fit_time", expiry, "min"), Real("svi_fit_time", expiry, "median"));
    EXPECT_EQ(Cell("svi_fit_time", expiry, "repetitions"), "2");
  }
  // SVI-8, the surface: the three fits and every check, so longer than any one slice's fit.
  const double surface = Real("svi_fit_surface_time", "surface", "min");
  EXPECT_GT(surface, 0.0);
  EXPECT_LE(surface, Real("svi_fit_surface_time", "surface", "median"));
  for (const std::string expiry : {"202411", "202411W4", "202412"}) {
    EXPECT_GT(surface, Real("svi_fit_time", expiry, "min")) << expiry;
  }
  EXPECT_EQ(Cell("svi_fit_surface_time", "surface", "repetitions"), "2");
}

TEST_F(SviFitTool, RunFlagsAreForTheReportOnly) {
  const RunResult result = RunTool("--reportable " + Arguments());
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.output.find("usage: aadp_svi_fit"), std::string::npos) << result.output;
}

TEST_F(SviFitTool, AReportableReportFromATreeThatIsNotCleanIsRefusedBeforeAnyWork) {
  const RunResult result =
      RunTool("--report --reportable --source-root=" + ShellArg((Dir() / "no-such-tree").string()) +
              " " + Arguments());
  EXPECT_EQ(WEXITSTATUS(result.status), 4) << result.output;
  EXPECT_NE(result.output.find("reportable run refused: tree is unknown"), std::string::npos)
      << result.output;
  EXPECT_EQ(result.output.find("aadp-results 1"), std::string::npos) << result.output;
}

#endif

}  // namespace
}  // namespace aadp
