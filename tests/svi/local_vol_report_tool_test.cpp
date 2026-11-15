// Runs the real aadp_local_vol_report binary on synthetic files made from hand-written SVI
// parameters, at reduced path counts. Nothing here comes from a TAIFEX file, and no number it
// prints is a result.

#include <gtest/gtest.h>
#include <sys/wait.h>  // IWYU pragma: keep

#include <algorithm>
#include <array>
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
#if AADP_REPORT_MODE
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
  const std::string command = ShellArg(AADP_LOCAL_VOL_REPORT_PATH) + " " + arguments +
                              (with_stderr ? " 2>&1" : " 2>/dev/null");
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

class LocalVolReportTool : public ::testing::Test {
 protected:
  void SetUp() override {
    // ctest runs each test as its own process, in parallel, so no two can share these files.
    dir_ = fs::path(AADP_LOCAL_VOL_REPORT_TEST_SCRATCH) /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::create_directories(dir_);
    WriteFile(dir_ / "options.csv",
              OptionsCsv(testing::SyntheticRows(Surface(), 20000.0, 25000.0, 100.0)));
    WriteFile(dir_ / "futures.csv", FuturesCsv());
  }

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
           ShellArg((dir_ / "futures.csv").string()) + " --paths 1024 --threads 2";
  }

  [[nodiscard]] const fs::path& Dir() const { return dir_; }

 private:
  fs::path dir_;
};

TEST_F(LocalVolReportTool, ReportOpensWithTheAttributionAndSaysIterationOnly) {
  const RunResult result = RunTool(Arguments());
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
  for (const char* expected : {"not tradable quotes", "Maturity reaches 33 days only",
                               "ITERATION ONLY, not a result", "run: seed 20261115, paths 1024"}) {
    EXPECT_NE(result.output.find(expected), std::string::npos) << expected << "\n" << result.output;
  }
}

TEST_F(LocalVolReportTool, ReportsEveryContractSensitivityFloorAndTheDiagnostic) {
  const RunResult result = RunTool(Arguments());
  EXPECT_EQ(result.status, 0) << result.output;
  for (const char* expected :
       {"== surface ==", "S0 = D1 F1", "== asian-arithmetic-call:",
        "== down-out-call-discrete-one-step:", "== down-out-call-continuous-bridge:",
        "d/d 202411   a", "d/d 202411W4 rho", "d/d 202412   sigma", "floors: path-steps",
        "paths with a floor", "floored, [T1, T2)", "capped, [T2, T3]",
        "== diagnostic, no tolerance: European call K 22800", "202411W4  12 dates: local vol"}) {
    EXPECT_NE(result.output.find(expected), std::string::npos) << expected << "\n" << result.output;
  }
  EXPECT_EQ(result.output.find("REFUSED"), std::string::npos) << result.output;
  EXPECT_EQ(result.output.find("timing"), std::string::npos) << result.output;
  EXPECT_EQ(result.output.find(" nan"), std::string::npos) << result.output;
  EXPECT_EQ(result.output.find("-nan"), std::string::npos) << result.output;
}

TEST_F(LocalVolReportTool, BiasCoversEveryTreatmentAndLevel) {
  const RunResult result = RunTool(Arguments() + " --mode bias");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
  for (const char* expected :
       {"Euler bias: ITERATION ONLY", "-- asian-arithmetic-call --",
        "-- down-out-call-discrete-one-step --", "-- down-out-call-discrete-hard --",
        "-- down-out-call-discrete-smoothed-0.005 --", "-- down-out-call-continuous-bridge --",
        "-- down-out-call-continuous-sampled-crossing --", "V(n = 16)", "Delta(8)",
        "bias left at n = 4"}) {
    EXPECT_NE(result.output.find(expected), std::string::npos) << expected << "\n" << result.output;
  }
}

TEST_F(LocalVolReportTool, TimingIsLabelledIterationOnly) {
  const RunResult result = RunTool(Arguments() + " --mode timing --repetitions 1");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
  EXPECT_NE(result.output.find("== timing: ITERATION ONLY, not a result =="), std::string::npos)
      << result.output;
}

TEST_F(LocalVolReportTool, MissingFileFailsAndSaysHowToGetIt) {
  const RunResult result = RunTool("--options " + ShellArg((Dir() / "absent.csv").string()) +
                                   " --futures " + ShellArg((Dir() / "futures.csv").string()));
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.output.find("scripts/fetch_data.sh"), std::string::npos) << result.output;
}

TEST_F(LocalVolReportTool, BadArgumentsAreAUsageError) {
  for (const char* bad : {"--paths 100", "--paths 0", "--mode fast", "--threads 0", "--paths"}) {
    const RunResult result = RunTool(bad);
    EXPECT_NE(result.status, 0) << bad;
    EXPECT_NE(result.output.find("usage: aadp_local_vol_report"), std::string::npos)
        << bad << "\n"
        << result.output;
  }
}

#if AADP_REPORT_MODE

// The comma-separated fields of every machine-readable line that starts with `kind,`, without the
// column header lines.
std::vector<std::vector<std::string>> Lines(const std::string& output, std::string_view kind) {
  std::vector<std::vector<std::string>> out;
  std::size_t at = 0;
  while (at < output.size()) {
    const std::size_t end = std::min(output.find('\n', at), output.size());
    const std::string line = output.substr(at, end - at);
    at = end + 1;
    if (!line.starts_with(std::string(kind) + ",") ||
        line.starts_with(std::string(kind) + ",contract,") ||
        line.starts_with(std::string(kind) + ",key,")) {
      continue;
    }
    std::vector<std::string> fields;
    std::size_t from = 0;
    while (true) {
      const std::size_t comma = line.find(',', from);
      fields.push_back(line.substr(from, comma == std::string::npos ? comma : comma - from));
      if (comma == std::string::npos) break;
      from = comma + 1;
    }
    out.push_back(std::move(fields));
  }
  return out;
}

double Full(const std::string& text) { return std::strtod(text.c_str(), nullptr); }

// One mode's text and report on the same inputs, the report read back by every rule.
class LocalVolReport : public LocalVolReportTool {
 protected:
  void Run(const std::string& mode_arguments) {
    text_ = RunTool(Arguments() + " " + mode_arguments);
    report_ = RunTool("--report " + Arguments() + " " + mode_arguments, false);
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

  [[nodiscard]] std::vector<std::string> Names() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const results::ReportTable& table : tables_) names.push_back(table.name);
    return names;
  }

  // Every table's columns, as table.column.
  [[nodiscard]] std::vector<std::string> Columns() const {
    std::vector<std::string> columns;
    for (const results::ReportTable& table : tables_) {
      for (const results::ReportColumn& column : table.columns) {
        columns.push_back(table.name + "." + column.name);
      }
    }
    return columns;
  }

  void ExpectTheFileShape(std::string_view mode) const {
    EXPECT_TRUE(report_.output.starts_with("aadp-results 1\n")) << report_.output;
    EXPECT_TRUE(results::missing_keys(file_, results::kRunHeaderKeys).empty());
    EXPECT_TRUE(results::missing_keys(file_, results::kPerfHeaderKeys).empty());
    EXPECT_EQ(file_.value("claim"), "result");
    EXPECT_EQ(results::file_kind(file_), results::FileKind::kReport);
    EXPECT_EQ(file_.value("report_tool"),
              "aadp_local_vol_report --report --mode " + std::string(mode));
    EXPECT_NE(file_.required("report_inputs").find("not tradable quotes"), std::string_view::npos);
    EXPECT_NE(file_.required("report_inputs").find("33 days only"), std::string_view::npos);
    ASSERT_FALSE(file_.sections.empty());
    EXPECT_EQ(file_.sections.front().name, "attribution");
    EXPECT_EQ(file_.sections.front().body, std::string(attribution_text()));
    for (const results::ReportTable& table : tables_) EXPECT_TRUE(table.uses_chain) << table.name;
  }

  [[nodiscard]] const RunResult& Text() const { return text_; }
  [[nodiscard]] const results::ResultsFile& File() const { return file_; }
  [[nodiscard]] const std::vector<results::ReportTable>& Tables() const { return tables_; }

 private:
  RunResult text_;
  RunResult report_;
  results::ResultsFile file_;
  std::vector<results::ReportTable> tables_;
};

class LocalVolPriceReport : public LocalVolReport {
 protected:
  void SetUp() override {
    LocalVolReport::SetUp();
    Run("");
  }

  // Every floors and range line of the text is a row of the report's floor tables.
  void ExpectFloorTablesHoldTheTextsCounts() const {
    const auto floors = Lines(Text().output, "floors");
    ASSERT_EQ(floors.size(), 3U);
    const std::vector<std::string> columns = {
        "path_steps", "paths",       "non_finite", "non_positive_variance", "calendar",
        "butterfly",  "below_floor", "capped",     "paths_with_floor",      "paths_with_cap"};
    for (const auto& f : floors) {
      ASSERT_EQ(f.size(), 2 + columns.size());
      for (std::size_t c = 0; c < columns.size(); ++c) {
        EXPECT_EQ(Cell("lv_floors", f[1], columns[c]), f[2 + c]) << f[1] << " " << columns[c];
      }
      EXPECT_EQ(f[2], std::to_string(1024 * 33 * 4)) << f[1];
    }
    const auto ranges = Lines(Text().output, "range");
    if (!ranges.empty()) {
      EXPECT_EQ(Table("lv_floor_ranges").rows.size(), ranges.size());
    }
    for (const auto& f : ranges) {
      ASSERT_EQ(f.size(), 9U);
      const std::string key = f[1] + "." + f[2] + "." + f[3];
      EXPECT_EQ(Cell("lv_floor_ranges", key, "count"), f[4]);
      EXPECT_EQ(Real("lv_floor_ranges", key, "k_min"), Full(f[5])) << key;
      EXPECT_EQ(Real("lv_floor_ranges", key, "k_max"), Full(f[6])) << key;
      EXPECT_EQ(Real("lv_floor_ranges", key, "t_min"), Full(f[7])) << key;
      EXPECT_EQ(Real("lv_floor_ranges", key, "t_max"), Full(f[8])) << key;
    }
  }
};

TEST_F(LocalVolPriceReport, IsOneResultsFileWithTheAttributionFirst) {
  ExpectTheFileShape("report");
  EXPECT_EQ(File().value("report_run"),
            "seed 20261115, paths 1024, batches 64, threads 2, 33 dates, n 4");
  EXPECT_EQ(File().value("repetitions"), "1");
}

TEST_F(LocalVolPriceReport, WritesThePricesTheSensitivitiesAndTheFloors) {
  std::vector<std::string> expected = {"lv_price", "lv_sensitivities", "lv_floors"};
  // A table without rows is not written: no floor or cap anywhere, no ranges table.
  if (!Lines(Text().output, "range").empty()) expected.emplace_back("lv_floor_ranges");
  EXPECT_EQ(Names(), expected);
  for (const results::ReportTable& table : Tables()) {
    EXPECT_FALSE(table.timing) << table.name;
    const bool floors = table.name.starts_with("lv_floor");
    EXPECT_EQ(table.targets,
              floors ? std::vector<std::string>{"SVI-9"} : std::vector<std::string>{})
        << table.name;
  }
}

// Derived figures only: no surface parameter, forward, discount or spot, and not the European
// diagnostic beside the fitted slice's Black-76 price at a listed strike.
TEST_F(LocalVolPriceReport, HoldsDerivedFiguresOnly) {
  std::vector<std::string> columns = Columns();
  std::erase_if(columns, [](const std::string& c) { return c.starts_with("lv_floor_ranges."); });
  EXPECT_EQ(columns, (std::vector<std::string>{
                         "lv_price.paths", "lv_price.price", "lv_price.price_se",
                         "lv_sensitivities.derivative", "lv_sensitivities.derivative_se",
                         "lv_floors.path_steps", "lv_floors.paths", "lv_floors.non_finite",
                         "lv_floors.non_positive_variance", "lv_floors.calendar",
                         "lv_floors.butterfly", "lv_floors.below_floor", "lv_floors.capped",
                         "lv_floors.paths_with_floor", "lv_floors.paths_with_cap"}));
  EXPECT_EQ(Table("lv_price").rows.size(), 3U);
  EXPECT_EQ(Table("lv_sensitivities").rows.size(), 45U);
  EXPECT_EQ(Table("lv_floors").rows.size(), 3U);
}

TEST_F(LocalVolPriceReport, ThePricesAndSensitivitiesAreTheTextsDoubles) {
  const auto prices = Lines(Text().output, "price");
  ASSERT_EQ(prices.size(), 3U);
  for (const auto& f : prices) {
    ASSERT_EQ(f.size(), 5U);
    EXPECT_EQ(Cell("lv_price", f[1], "paths"), f[2]);
    EXPECT_EQ(Real("lv_price", f[1], "price"), Full(f[3])) << f[1];
    EXPECT_EQ(Real("lv_price", f[1], "price_se"), Full(f[4])) << f[1];
  }
  const auto sensitivities = Lines(Text().output, "sensitivity");
  ASSERT_EQ(sensitivities.size(), 45U);
  for (const auto& f : sensitivities) {
    ASSERT_EQ(f.size(), 6U);
    const std::string key = f[1] + "." + f[2] + "." + f[3];
    EXPECT_EQ(Real("lv_sensitivities", key, "derivative"), Full(f[4])) << key;
    EXPECT_EQ(Real("lv_sensitivities", key, "derivative_se"), Full(f[5])) << key;
  }
}

TEST_F(LocalVolPriceReport, TheFloorTablesHoldTheTextsCounts) {
  ExpectFloorTablesHoldTheTextsCounts();
}

// 202411's total variance raised above 202411W4's near the money: the slices cross, so the
// paths floor between T1 and T2 and the range table is written.
class LocalVolCrossingReport : public LocalVolPriceReport {
 protected:
  [[nodiscard]] std::vector<testing::SyntheticExpiry> Surface() const override {
    std::vector<testing::SyntheticExpiry> surface = LocalVolPriceReport::Surface();
    surface[0].params.a += 8e-4;
    return surface;
  }
};

TEST_F(LocalVolCrossingReport, WritesWhereTheFloorsFellWithTheTextsDoubles) {
  ASSERT_FALSE(Lines(Text().output, "range").empty()) << Text().output;
  EXPECT_NE(Cell("lv_floors", "asian-arithmetic-call", "calendar"), "0");
  EXPECT_EQ(Names().back(), "lv_floor_ranges");
  ExpectFloorTablesHoldTheTextsCounts();
}

class LocalVolBiasReport : public LocalVolReport {
 protected:
  void SetUp() override {
    LocalVolReport::SetUp();
    Run("--mode bias");
  }
};

TEST_F(LocalVolBiasReport, IsOneResultsFileWithTheFourBiasTables) {
  ExpectTheFileShape("bias");
  EXPECT_EQ(File().value("report_run"),
            "seed 20261115, paths 1024, batches 64, threads 2, 33 dates, n 1 to 16, coupled draws");
  EXPECT_EQ(Names(), (std::vector<std::string>{"lv_bias_levels", "lv_bias_delta", "lv_bias_ratio",
                                               "lv_bias_left"}));
  for (const results::ReportTable& table : Tables()) {
    EXPECT_FALSE(table.timing) << table.name;
    EXPECT_TRUE(table.targets.empty()) << table.name;
  }
  EXPECT_EQ(Table("lv_bias_levels").rows.size(), 30U);
  EXPECT_EQ(Table("lv_bias_delta").rows.size(), 24U);
  EXPECT_EQ(Table("lv_bias_ratio").rows.size(), 18U);
  EXPECT_EQ(Table("lv_bias_left").rows.size(), 6U);
}

TEST_F(LocalVolBiasReport, TheBiasTablesHoldTheTextsDoubles) {
  const auto levels = Lines(Text().output, "level");
  ASSERT_EQ(levels.size(), 30U);
  for (const auto& f : levels) {
    ASSERT_EQ(f.size(), 5U);
    const std::string key = f[1] + ".n" + f[2];
    EXPECT_EQ(Cell("lv_bias_levels", key, "substeps"), f[2]);
    EXPECT_EQ(Cell("lv_bias_levels", key, "draws"), f[2] == "1" ? "coarse" : "production");
    EXPECT_EQ(Real("lv_bias_levels", key, "value"), Full(f[3])) << key;
    EXPECT_EQ(Real("lv_bias_levels", key, "value_se"), Full(f[4])) << key;
  }
  const auto deltas = Lines(Text().output, "delta");
  ASSERT_EQ(deltas.size(), 24U);
  for (const auto& f : deltas) {
    ASSERT_EQ(f.size(), 5U);
    const std::string key = f[1] + ".n" + f[2];
    EXPECT_EQ(Cell("lv_bias_delta", key, "substeps"), f[2]);
    EXPECT_EQ(Real("lv_bias_delta", key, "delta"), Full(f[3])) << key;
    EXPECT_EQ(Real("lv_bias_delta", key, "delta_se"), Full(f[4])) << key;
  }
  const auto ratios = Lines(Text().output, "ratio");
  ASSERT_EQ(ratios.size(), 18U);
  for (const auto& f : ratios) {
    ASSERT_EQ(f.size(), 4U);
    const std::string key = f[1] + ".n" + f[2];
    EXPECT_EQ(Real("lv_bias_ratio", key, "ratio"), Full(f[3])) << key;
  }
  const auto left = Lines(Text().output, "left");
  ASSERT_EQ(left.size(), 6U);
  for (const auto& f : left) {
    ASSERT_EQ(f.size(), 5U);
    EXPECT_EQ(Real("lv_bias_left", f[1], "bias"), Full(f[2])) << f[1];
    EXPECT_EQ(Real("lv_bias_left", f[1], "bias_se"), Full(f[3])) << f[1];
    EXPECT_EQ(Cell("lv_bias_left", f[1], "resolved"), f[4] == "1" ? "yes" : "no") << f[1];
    // The verdict is the text's: "not resolved at this N" exactly where the report says no.
    const std::size_t block = Text().output.find("-- " + f[1] + " --");
    ASSERT_NE(block, std::string::npos) << f[1];
    const std::size_t line = Text().output.find("bias left at n = 4", block);
    const std::size_t end = Text().output.find('\n', line);
    const bool unresolved = Text().output.substr(line, end - line).contains("not resolved");
    EXPECT_EQ(unresolved, f[4] == "0") << f[1];
  }
}

TEST_F(LocalVolReportTool, TheTimingReportIsOneTimedAadOneTable) {
  const RunResult result = RunTool("--report --mode timing --repetitions 2 " + Arguments(), false);
  ASSERT_EQ(result.status, 0) << result.output;
  auto file = results::parse_results_file(result.output);
  ASSERT_TRUE(file.has_value()) << file.error();
  EXPECT_EQ(file->value("repetitions"), "2");
  EXPECT_EQ(file->sections.front().body, std::string(attribution_text()));
  const auto tables = results::read_report(*file);
  ASSERT_TRUE(tables.has_value()) << tables.error().detail;
  ASSERT_EQ(tables->size(), 1U);
  const results::ReportTable& timing = tables->front();
  EXPECT_EQ(timing.name, "lv_time");
  EXPECT_EQ(timing.targets, (std::vector<std::string>{"AAD-1"}));
  EXPECT_TRUE(timing.timing);
  EXPECT_TRUE(timing.uses_chain);
  ASSERT_EQ(timing.rows.size(), 3U);
  for (const results::ReportRow& row : timing.rows) {
    const auto text = [&](std::string_view column) {
      const auto index = timing.column(column);
      return index ? row.cells[*index] : std::string();
    };
    const auto cell = [&](std::string_view column) {
      return results::read_real_cell(text(column)).value_or(-1.0);
    };
    EXPECT_GT(cell("price_min"), 0.0) << row.key;
    EXPECT_LE(cell("price_min"), cell("price_median")) << row.key;
    EXPECT_GT(cell("gradient_min"), 0.0) << row.key;
    EXPECT_LE(cell("gradient_min"), cell("gradient_median")) << row.key;
    EXPECT_EQ(cell("ratio"), cell("gradient_median") / cell("price_median")) << row.key;
    EXPECT_EQ(text("repetitions"), "2") << row.key;
  }
}

TEST_F(LocalVolReportTool, RunFlagsAreForTheReportOnly) {
  const RunResult result = RunTool("--reportable " + Arguments());
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.output.find("usage: aadp_local_vol_report"), std::string::npos) << result.output;
}

TEST_F(LocalVolReportTool, AReportableReportFromATreeThatIsNotCleanIsRefusedBeforeAnyWork) {
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
