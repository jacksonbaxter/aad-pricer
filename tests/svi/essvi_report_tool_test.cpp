// Runs the real aadp_essvi_report binary on synthetic files made from hand-written eSSVI
// parameters (docs/NOTES.md, "Stage 7c": H, converted to raw SVI), at reduced path counts. Nothing
// here comes from a TAIFEX file, and no number it prints is a result.

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
#include "svi/essvi.hpp"
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
  const std::string command =
      ShellArg(AADP_ESSVI_REPORT_PATH) + " " + arguments + (with_stderr ? " 2>&1" : " 2>/dev/null");
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

class EssviReportTool : public ::testing::Test {
 protected:
  void SetUp() override {
    // ctest runs each test as its own process, in parallel, so no two can share these files.
    dir_ = fs::path(AADP_ESSVI_REPORT_TEST_SCRATCH) /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::create_directories(dir_);
    WriteFile(dir_ / "options.csv",
              OptionsCsv(testing::SyntheticRows(Surface(), 20000.0, 25000.0, 100.0)));
    WriteFile(dir_ / "futures.csv", FuturesCsv());
  }

  // H, the pre-registered hand-made surface, as raw SVI.
  [[nodiscard]] virtual std::vector<testing::SyntheticExpiry> Surface() const {
    return {{.expiry = "202411",
             .forward = 22800.0,
             .discount = 0.9999,
             .params =
                 svi::ToRawSvi(svi::EssviSlice<double>{.theta = 4.4e-4, .rho = -0.5, .psi = 0.02})},
            {.expiry = "202411W4",
             .forward = 22810.0,
             .discount = 0.9995,
             .params =
                 svi::ToRawSvi(svi::EssviSlice<double>{.theta = 1e-3, .rho = -0.45, .psi = 0.03})},
            {.expiry = "202412",
             .forward = 22830.0,
             .discount = 0.998,
             .params = svi::ToRawSvi(
                 svi::EssviSlice<double>{.theta = 2.8e-3, .rho = -0.5, .psi = 0.05})}};
  }

  [[nodiscard]] std::string Arguments() const {
    return "--options " + ShellArg((dir_ / "options.csv").string()) + " --futures " +
           ShellArg((dir_ / "futures.csv").string());
  }

  [[nodiscard]] const fs::path& Dir() const { return dir_; }

 private:
  fs::path dir_;
};

// The number just after label, which must start at text[at]; NaN if there is none.
double NumberAfter(const std::string& text, std::size_t at, std::string_view label) {
  if (at == std::string::npos || text.compare(at, label.size(), label) != 0) return std::nan("");
  const char* begin = text.c_str() + at + label.size();
  char* end = nullptr;
  const double value = std::strtod(begin, &end);
  return end == begin ? std::nan("") : value;
}

// The two counts of ": path-steps <steps> over <paths> paths" at text[at]; zeros if not there.
std::pair<unsigned long long, unsigned long long> PathSteps(const std::string& text,
                                                            std::size_t at) {
  constexpr std::string_view kSteps = ": path-steps ";
  constexpr std::string_view kOver = " over ";
  if (text.compare(at, kSteps.size(), kSteps) != 0) return {0, 0};
  const char* begin = text.c_str() + at + kSteps.size();
  char* end = nullptr;
  const unsigned long long steps = std::strtoull(begin, &end, 10);
  if (end == begin || std::string_view(end, kOver.size()) != kOver) return {0, 0};
  begin = end + kOver.size();
  const unsigned long long paths = std::strtoull(begin, &end, 10);
  return end == begin ? std::pair<unsigned long long, unsigned long long>{0, 0}
                      : std::pair{steps, paths};
}

void ExpectNoNan(const RunResult& result) {
  EXPECT_EQ(result.output.find(" nan"), std::string::npos) << result.output;
  EXPECT_EQ(result.output.find("-nan"), std::string::npos) << result.output;
}

TEST_F(EssviReportTool, FitOpensWithTheAttributionAndReportsBothFits) {
  const RunResult result = RunTool(Arguments());
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
  for (const char* expected : {"not tradable quotes",
                               "Maturity reaches 33 days only",
                               "ITERATION ONLY, not a result",
                               "== eSSVI surface fit:",
                               "converged yes",
                               "start  0 (Mingone's)",
                               "start 15",
                               "constraints on the fitted (theta, rho, psi): 0 violated",
                               "B1",
                               "== slice 202411W4:",
                               "eSSVI as raw SVI",
                               "psi interval",
                               "raw SVI (6b)",
                               "Phi/(n-3)",
                               "Phi/(n-5)",
                               "SVI-6 (bp of F)",
                               "SVI-11 (vol points)",
                               "eSSVI butterfly",
                               "raw SVI butterfly",
                               "== calendar, adjacent slices ==",
                               "eSSVI calendar",
                               "== surface ==",
                               "Phi/(",
                               "-9)",
                               "== machine-readable ==",
                               "essvi,202412,"}) {
    EXPECT_NE(result.output.find(expected), std::string::npos) << expected << "\n" << result.output;
  }
  EXPECT_EQ(result.output.find("FAILED"), std::string::npos) << result.output;
  EXPECT_EQ(result.output.find("VIOLATED"), std::string::npos) << result.output;
  ExpectNoNan(result);
}

TEST_F(EssviReportTool, PriceGivesBothLinesThePairedDifferenceAndBothFloorReports) {
  const RunResult result = RunTool(Arguments() + " --mode price --paths 1024 --threads 2");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
  for (const char* expected :
       {"run: seed 20261115, paths 1024, batches 64, threads 2", "raw SVI surface, S0 = D1 F1",
        "eSSVI   surface, S0 = D1 F1", "== asian-arithmetic-call:",
        "== down-out-call-discrete-one-step:", "== down-out-call-continuous-bridge:",
        "raw SVI (7a)", "paired eSSVI - raw", "from 64 batch differences",
        "floors, eSSVI:", "floors, raw SVI:", "floored, [T1, T2)", "capped, [T2, T3]"}) {
    EXPECT_NE(result.output.find(expected), std::string::npos) << expected << "\n" << result.output;
  }
  EXPECT_EQ(result.output.find("REFUSED"), std::string::npos) << result.output;
  ExpectNoNan(result);
  // Per contract: the paired difference is the difference of the two means (the mean of the batch
  // differences, to the printed digits), and each floor report covers every path-step: 1,024
  // paths of 33 dates and n = 4.
  std::size_t contracts = 0;
  for (std::size_t at = result.output.find("  raw SVI (7a)"); at != std::string::npos;
       at = result.output.find("  raw SVI (7a)", at + 1)) {
    const double raw = NumberAfter(result.output, at, "  raw SVI (7a)");
    const double essvi =
        NumberAfter(result.output, result.output.find("  eSSVI    ", at), "  eSSVI");
    const double paired = NumberAfter(result.output, result.output.find("  paired eSSVI - raw", at),
                                      "  paired eSSVI - raw");
    ASSERT_TRUE(std::isfinite(raw) && std::isfinite(essvi) && std::isfinite(paired))
        << result.output.substr(at, 400);
    EXPECT_NEAR(paired, essvi - raw, 2e-6) << result.output.substr(at, 400);
    ++contracts;
  }
  EXPECT_EQ(contracts, 3U);
  std::size_t reports = 0;
  for (std::size_t at = result.output.find("    floors, "); at != std::string::npos;
       at = result.output.find("    floors, ", at + 1)) {
    const std::size_t colon = result.output.find(": path-steps", at);
    ASSERT_NE(colon, std::string::npos);
    const auto [steps, paths] = PathSteps(result.output, colon);
    EXPECT_EQ(steps, 1024ULL * 33ULL * 4ULL);
    EXPECT_EQ(paths, 1024ULL);
    ++reports;
  }
  EXPECT_EQ(reports, 6U);
}

TEST_F(EssviReportTool, TimingIsLabelledIterationOnly) {
  const RunResult result = RunTool(Arguments() + " --mode timing --repetitions 1");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
  for (const char* expected : {"== timing: ITERATION ONLY, not a result (SVI-8, eSSVI surface) ==",
                               "SVI-8 eSSVI fit alone", "SVI-8 eSSVI fit and checks"}) {
    EXPECT_NE(result.output.find(expected), std::string::npos) << expected << "\n" << result.output;
  }
}

TEST_F(EssviReportTool, MissingFileFailsAndSaysHowToGetIt) {
  const RunResult result = RunTool("--options " + ShellArg((Dir() / "absent.csv").string()) +
                                   " --futures " + ShellArg((Dir() / "futures.csv").string()));
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.output.find("scripts/fetch_data.sh"), std::string::npos) << result.output;
}

TEST_F(EssviReportTool, BadArgumentsAreAUsageError) {
  for (const char* bad :
       {"--paths 100", "--paths 0", "--mode fast", "--threads 0", "--paths", "--repetitions 0"}) {
    const RunResult result = RunTool(bad);
    EXPECT_NE(result.status, 0) << bad;
    EXPECT_NE(result.output.find("usage: aadp_essvi_report"), std::string::npos) << bad << "\n"
                                                                                 << result.output;
  }
}

#if AADP_REPORT_MODE

// The comma-separated fields of every machine-readable line that starts with `kind,`, without
// the column header lines.
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
    constexpr std::array<std::string_view, 6> kHeaders = {"expiry",    "key",      "slice",
                                                          "converged", "contract", "quotes"};
    if (fields.size() > 1 && std::ranges::find(kHeaders, fields[1]) != kHeaders.end()) continue;
    out.push_back(std::move(fields));
  }
  return out;
}

double Full(const std::string& text) { return std::strtod(text.c_str(), nullptr); }

const char* Flag(const std::string& text) { return text == "1" ? "yes" : "no"; }

// The report's position cells: a real cell's spelling, or +inf, -inf or none.
std::string Position(const std::string& text) {
  const double k = Full(text);
  if (std::isnan(k)) return "none";
  if (std::isinf(k)) return k > 0.0 ? "+inf" : "-inf";
  return results::real_cell(k);
}

// One mode's text and report on the same inputs, the report read back by every rule.
class EssviReport : public EssviReportTool {
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

  [[nodiscard]] const results::ReportTable* Find(std::string_view name) const {
    for (const results::ReportTable& table : tables_) {
      if (table.name == name) return &table;
    }
    return nullptr;
  }

  [[nodiscard]] const results::ReportTable& Table(std::string_view name) const {
    const results::ReportTable* table = Find(name);
    if (table == nullptr) {
      ADD_FAILURE() << "no table " << name;
      return tables_.front();
    }
    return *table;
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

  void ExpectTheFileShape() const {
    EXPECT_TRUE(Report().output.starts_with("aadp-results 1\n")) << Report().output;
    EXPECT_TRUE(results::missing_keys(File(), results::kRunHeaderKeys).empty());
    EXPECT_TRUE(results::missing_keys(File(), results::kPerfHeaderKeys).empty());
    EXPECT_EQ(File().value("claim"), "result");
    EXPECT_EQ(results::file_kind(File()), results::FileKind::kReport);
    EXPECT_NE(File().required("report_inputs").find("not tradable quotes"), std::string_view::npos);
    EXPECT_NE(File().required("report_inputs").find("33 days only"), std::string_view::npos);
    ASSERT_FALSE(File().sections.empty());
    EXPECT_EQ(File().sections.front().name, "attribution");
    EXPECT_EQ(File().sections.front().body, std::string(attribution_text()));
    for (const results::ReportTable& table : Tables()) EXPECT_TRUE(table.uses_chain) << table.name;
  }

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

class EssviFitReport : public EssviReport {
 protected:
  void SetUp() override {
    EssviReport::SetUp();
    Run("--mode fit");
  }
};

TEST_F(EssviFitReport, IsOneResultsFileWithTheRunHeaderAndTheAttributionFirst) {
  ExpectTheFileShape();
  EXPECT_EQ(File().value("report_tool"), "aadp_essvi_report --report --mode fit");
}

TEST_F(EssviFitReport, WritesExactlyTheSviNineTables) {
  std::vector<std::string> expected = {"essvi_fit_error", "essvi_fit_surface",
                                       "essvi_fit_arbitrage", "essvi_fit_calendar"};
  // A table without rows is not written: no violation, no violations table.
  if (!Lines(Text().output, "violation").empty()) expected.emplace_back("essvi_fit_violations");
  expected.emplace_back("essvi_fit_constraints");
  expected.emplace_back("essvi_fit_convergence");
  EXPECT_EQ(Names(), expected);
  for (const results::ReportTable& table : Tables()) {
    EXPECT_EQ(table.targets, (std::vector<std::string>{"SVI-9"})) << table.name;
    EXPECT_FALSE(table.timing) << table.name;
  }
}

// Derived figures only: no column a settlement, a strike, the forward, the discount or a fitted
// parameter could be read from, and rows per slice, pair, surface or constraint, never per strike.
TEST_F(EssviFitReport, HoldsDerivedFiguresOnly) {
  std::vector<std::string> columns;
  for (const results::ReportTable& table : Tables()) {
    if (table.name == "essvi_fit_violations") continue;
    for (const results::ReportColumn& column : table.columns) {
      columns.push_back(table.name + "." + column.name);
    }
  }
  EXPECT_EQ(columns, (std::vector<std::string>{"essvi_fit_error.quotes",
                                               "essvi_fit_error.essvi_phi",
                                               "essvi_fit_error.essvi_phi_per_dof",
                                               "essvi_fit_error.raw_phi",
                                               "essvi_fit_error.raw_phi_per_dof",
                                               "essvi_fit_error.phi_ratio",
                                               "essvi_fit_error.essvi_svi6",
                                               "essvi_fit_error.raw_svi6",
                                               "essvi_fit_error.essvi_svi11",
                                               "essvi_fit_error.raw_svi11",
                                               "essvi_fit_surface.quotes",
                                               "essvi_fit_surface.essvi_phi",
                                               "essvi_fit_surface.essvi_phi_per_dof",
                                               "essvi_fit_surface.raw_phi",
                                               "essvi_fit_surface.raw_phi_per_dof",
                                               "essvi_fit_surface.phi_ratio",
                                               "essvi_fit_arbitrage.model_triples",
                                               "essvi_fit_arbitrage.model_non_convex",
                                               "essvi_fit_arbitrage.grid_points",
                                               "essvi_fit_arbitrage.min_g",
                                               "essvi_fit_arbitrage.k_min_g",
                                               "essvi_fit_arbitrage.martini_mingone",
                                               "essvi_fit_arbitrage.violations",
                                               "essvi_fit_calendar.grid_points",
                                               "essvi_fit_calendar.min_gap",
                                               "essvi_fit_calendar.k_min_gap",
                                               "essvi_fit_calendar.violations",
                                               "essvi_fit_constraints.slack",
                                               "essvi_fit_constraints.active",
                                               "essvi_fit_constraints.violated",
                                               "essvi_fit_convergence.converged",
                                               "essvi_fit_convergence.stop",
                                               "essvi_fit_convergence.iterations",
                                               "essvi_fit_convergence.scaled_gradient",
                                               "essvi_fit_convergence.starts_at_best",
                                               "essvi_fit_convergence.starts",
                                               "essvi_fit_convergence.constraints_violated",
                                               "essvi_fit_convergence.constraints_active"}));
  EXPECT_EQ(Table("essvi_fit_error").rows.size(), 3U);
  EXPECT_EQ(Table("essvi_fit_surface").rows.size(), 1U);
  EXPECT_EQ(Table("essvi_fit_arbitrage").rows.size(), 6U);
  EXPECT_EQ(Table("essvi_fit_calendar").rows.size(), 4U);
  EXPECT_EQ(Table("essvi_fit_constraints").rows.size(), Lines(Text().output, "constraint").size());
  EXPECT_EQ(Table("essvi_fit_convergence").rows.size(), 1U);
}

TEST_F(EssviFitReport, TheErrorTablesHoldTheTextsDoubles) {
  const auto essvi = Lines(Text().output, "essvi");
  const auto raw = Lines(Text().output, "raw");
  ASSERT_EQ(essvi.size(), 3U);
  ASSERT_EQ(raw.size(), 3U);
  double raw_total = 0.0;
  std::size_t quotes = 0;
  for (std::size_t j = 0; j < 3; ++j) {
    const auto& e = essvi[j];
    const auto& r = raw[j];
    ASSERT_EQ(e.size(), 15U);
    ASSERT_EQ(r.size(), 6U);
    const std::string& expiry = e[1];
    ASSERT_EQ(r[1], expiry);
    const double n = Full(e[2]);
    const double e_phi = Full(e[11]);
    const double r_phi = Full(r[3]);
    raw_total += r_phi;
    quotes += static_cast<std::size_t>(n);
    EXPECT_EQ(Cell("essvi_fit_error", expiry, "quotes"), e[2]);
    EXPECT_EQ(Real("essvi_fit_error", expiry, "essvi_phi"), e_phi) << expiry;
    EXPECT_EQ(Real("essvi_fit_error", expiry, "essvi_phi_per_dof"), e_phi / (n - 3.0)) << expiry;
    EXPECT_EQ(Real("essvi_fit_error", expiry, "raw_phi"), r_phi) << expiry;
    EXPECT_EQ(Real("essvi_fit_error", expiry, "raw_phi_per_dof"), r_phi / (n - 5.0)) << expiry;
    EXPECT_EQ(Real("essvi_fit_error", expiry, "phi_ratio"), e_phi / r_phi) << expiry;
    EXPECT_EQ(Real("essvi_fit_error", expiry, "essvi_svi6"), Full(e[12])) << expiry;
    EXPECT_EQ(Real("essvi_fit_error", expiry, "raw_svi6"), Full(r[4])) << expiry;
    EXPECT_EQ(Real("essvi_fit_error", expiry, "essvi_svi11"), Full(e[13])) << expiry;
    EXPECT_EQ(Real("essvi_fit_error", expiry, "raw_svi11"), Full(r[5])) << expiry;
  }
  const auto n = static_cast<double>(quotes);
  EXPECT_EQ(Cell("essvi_fit_surface", "surface", "quotes"), std::to_string(quotes));
  EXPECT_EQ(Real("essvi_fit_surface", "surface", "raw_phi"), raw_total);
  EXPECT_EQ(Real("essvi_fit_surface", "surface", "raw_phi_per_dof"), raw_total / (n - 15.0));
  const auto surface = Lines(Text().output, "surface");
  ASSERT_EQ(surface.size(), 1U);
  ASSERT_EQ(surface.front().size(), 3U);
  EXPECT_EQ(surface.front()[1], std::to_string(quotes));
  const double surface_phi = Full(surface.front()[2]);
  EXPECT_EQ(Real("essvi_fit_surface", "surface", "essvi_phi"), surface_phi);
  EXPECT_EQ(Real("essvi_fit_surface", "surface", "essvi_phi_per_dof"), surface_phi / (n - 9.0));
  EXPECT_EQ(Real("essvi_fit_surface", "surface", "phi_ratio"), surface_phi / raw_total);
}

TEST_F(EssviFitReport, TheCheckTablesHoldTheTextsDoubles) {
  const auto arbitrage = Lines(Text().output, "arbitrage");
  ASSERT_EQ(arbitrage.size(), 6U);
  for (const auto& f : arbitrage) {
    ASSERT_EQ(f.size(), 9U);
    const std::string& key = f[1];
    EXPECT_EQ(Cell("essvi_fit_arbitrage", key, "model_triples"), f[2]);
    EXPECT_EQ(Cell("essvi_fit_arbitrage", key, "model_non_convex"), f[3]);
    EXPECT_EQ(Cell("essvi_fit_arbitrage", key, "grid_points"), f[4]);
    EXPECT_EQ(Real("essvi_fit_arbitrage", key, "min_g"), Full(f[5])) << key;
    EXPECT_EQ(Real("essvi_fit_arbitrage", key, "k_min_g"), Full(f[6])) << key;
    EXPECT_EQ(Cell("essvi_fit_arbitrage", key, "martini_mingone"), f[7]);
    EXPECT_EQ(Cell("essvi_fit_arbitrage", key, "violations"), f[8]);
  }
  const auto calendar = Lines(Text().output, "calendar");
  ASSERT_EQ(calendar.size(), 4U);
  for (const auto& f : calendar) {
    ASSERT_EQ(f.size(), 6U);
    const std::string& key = f[1];
    EXPECT_EQ(Cell("essvi_fit_calendar", key, "grid_points"), f[2]);
    EXPECT_EQ(Real("essvi_fit_calendar", key, "min_gap"), Full(f[3])) << key;
    EXPECT_EQ(Real("essvi_fit_calendar", key, "k_min_gap"), Full(f[4])) << key;
    EXPECT_EQ(Cell("essvi_fit_calendar", key, "violations"), f[5]);
  }
  const auto violations = Lines(Text().output, "violation");
  if (!violations.empty()) {
    EXPECT_EQ(Table("essvi_fit_violations").rows.size(), violations.size());
  }
  for (const auto& f : violations) {
    ASSERT_EQ(f.size(), 10U);
    const std::string key = f[1] + "." + f[2];
    EXPECT_EQ(Cell("essvi_fit_violations", key, "kind"), f[3]);
    EXPECT_EQ(Cell("essvi_fit_violations", key, "k_begin"), Position(f[4])) << key;
    EXPECT_EQ(Cell("essvi_fit_violations", key, "k_end"), Position(f[5])) << key;
    EXPECT_EQ(Cell("essvi_fit_violations", key, "k_worst"), Position(f[6])) << key;
    EXPECT_EQ(Real("essvi_fit_violations", key, "value"), Full(f[7])) << key;
    EXPECT_EQ(Real("essvi_fit_violations", key, "bound"), Full(f[8])) << key;
    EXPECT_EQ(Cell("essvi_fit_violations", key, "points"), f[9]);
  }
  const auto constraints = Lines(Text().output, "constraint");
  ASSERT_FALSE(constraints.empty());
  for (const auto& f : constraints) {
    ASSERT_EQ(f.size(), 6U);
    const std::string key = f[1] + "." + f[2];
    EXPECT_EQ(Real("essvi_fit_constraints", key, "slack"), Full(f[3])) << key;
    EXPECT_EQ(Cell("essvi_fit_constraints", key, "active"), Flag(f[4])) << key;
    EXPECT_EQ(Cell("essvi_fit_constraints", key, "violated"), Flag(f[5])) << key;
  }
  const auto convergence = Lines(Text().output, "convergence");
  ASSERT_EQ(convergence.size(), 1U);
  const auto& c = convergence.front();
  ASSERT_EQ(c.size(), 9U);
  EXPECT_EQ(Cell("essvi_fit_convergence", "surface", "converged"), Flag(c[1]));
  EXPECT_EQ(Cell("essvi_fit_convergence", "surface", "stop"), c[2]);
  EXPECT_EQ(Cell("essvi_fit_convergence", "surface", "iterations"), c[3]);
  EXPECT_EQ(Real("essvi_fit_convergence", "surface", "scaled_gradient"), Full(c[4]));
  EXPECT_EQ(Cell("essvi_fit_convergence", "surface", "starts_at_best"), c[5]);
  EXPECT_EQ(Cell("essvi_fit_convergence", "surface", "starts"), c[6]);
  EXPECT_EQ(Cell("essvi_fit_convergence", "surface", "constraints_violated"), c[7]);
  EXPECT_EQ(Cell("essvi_fit_convergence", "surface", "constraints_active"), c[8]);
}

// 202411W4's total variance below 202411's near the money: the raw slices cross, unrepaired, so
// the raw surface has calendar violations and its local variance floors, while eSSVI has neither.
class EssviCrossingSurface : public EssviReport {
 protected:
  [[nodiscard]] std::vector<testing::SyntheticExpiry> Surface() const override {
    std::vector<testing::SyntheticExpiry> surface = EssviReport::Surface();
    surface[0].params.a += 6e-4;
    return surface;
  }
};

class EssviCrossingFitReport : public EssviCrossingSurface {
 protected:
  void SetUp() override {
    EssviCrossingSurface::SetUp();
    Run("--mode fit");
  }
};

TEST_F(EssviCrossingFitReport, ListsTheRawSurfacesViolationsWithTheTextsDoubles) {
  ASSERT_FALSE(Lines(Text().output, "violation").empty()) << Text().output;
  EXPECT_NE(Cell("essvi_fit_calendar", "202411-202411W4.raw", "violations"), "0");
  EXPECT_EQ(Table("essvi_fit_violations").rows.size(), Lines(Text().output, "violation").size());
}

class EssviPriceReport : public EssviCrossingSurface {
 protected:
  void SetUp() override {
    EssviCrossingSurface::SetUp();
    Run("--mode price --paths 1024 --threads 2");
  }
};

TEST_F(EssviPriceReport, IsOneResultsFileWithThePricingTables) {
  ExpectTheFileShape();
  EXPECT_EQ(File().value("report_tool"), "aadp_essvi_report --report --mode price");
  EXPECT_EQ(File().value("report_run"),
            "seed 20261115, paths 1024, batches 64, threads 2, n 4, "
            "33 dates");
  EXPECT_EQ(Names(), (std::vector<std::string>{"essvi_price", "essvi_price_floors",
                                               "essvi_price_floor_ranges"}));
  for (const results::ReportTable& table : Tables()) {
    EXPECT_EQ(table.targets, (std::vector<std::string>{"SVI-9"})) << table.name;
    EXPECT_FALSE(table.timing) << table.name;
  }
  EXPECT_EQ(Table("essvi_price").rows.size(), 3U);
  EXPECT_EQ(Table("essvi_price_floors").rows.size(), 6U);
}

TEST_F(EssviPriceReport, ThePriceTableHoldsTheTextsDoubles) {
  const auto prices = Lines(Text().output, "price");
  ASSERT_EQ(prices.size(), 3U);
  for (const auto& f : prices) {
    ASSERT_EQ(f.size(), 9U);
    const std::string& key = f[1];
    EXPECT_EQ(Cell("essvi_price", key, "paths"), "1024");
    EXPECT_EQ(Real("essvi_price", key, "raw"), Full(f[2])) << key;
    EXPECT_EQ(Real("essvi_price", key, "raw_se"), Full(f[3])) << key;
    EXPECT_EQ(Real("essvi_price", key, "essvi"), Full(f[4])) << key;
    EXPECT_EQ(Real("essvi_price", key, "essvi_se"), Full(f[5])) << key;
    EXPECT_EQ(Real("essvi_price", key, "paired_difference"), Full(f[6])) << key;
    EXPECT_EQ(Real("essvi_price", key, "paired_se"), Full(f[7])) << key;
    EXPECT_EQ(Cell("essvi_price", key, "paired_batches"), f[8]);
  }
}

TEST_F(EssviPriceReport, TheFloorTablesHoldTheTextsCounts) {
  const auto floors = Lines(Text().output, "floors");
  ASSERT_EQ(floors.size(), 6U);
  const std::vector<std::string> columns = {
      "path_steps", "paths",       "non_finite", "non_positive_variance", "calendar",
      "butterfly",  "below_floor", "capped",     "paths_with_floor",      "paths_with_cap"};
  for (const auto& f : floors) {
    ASSERT_EQ(f.size(), 2 + columns.size());
    for (std::size_t c = 0; c < columns.size(); ++c) {
      EXPECT_EQ(Cell("essvi_price_floors", f[1], columns[c]), f[2 + c])
          << f[1] << " " << columns[c];
    }
    // Every path-step of 1,024 paths of 33 dates at n = 4 is counted.
    EXPECT_EQ(f[2], std::to_string(1024 * 33 * 4)) << f[1];
  }
  const auto ranges = Lines(Text().output, "range");
  // The raw surface crosses, so its floors fall somewhere, and only its ranges are written.
  ASSERT_FALSE(ranges.empty()) << Text().output;
  EXPECT_EQ(Table("essvi_price_floor_ranges").rows.size(), ranges.size());
  for (const auto& f : ranges) {
    ASSERT_EQ(f.size(), 9U);
    EXPECT_TRUE(f[1].ends_with(".raw")) << f[1];
    const std::string key = f[1] + "." + f[2] + "." + f[3];
    EXPECT_EQ(Cell("essvi_price_floor_ranges", key, "count"), f[4]);
    EXPECT_EQ(Real("essvi_price_floor_ranges", key, "k_min"), Full(f[5])) << key;
    EXPECT_EQ(Real("essvi_price_floor_ranges", key, "k_max"), Full(f[6])) << key;
    EXPECT_EQ(Real("essvi_price_floor_ranges", key, "t_min"), Full(f[7])) << key;
    EXPECT_EQ(Real("essvi_price_floor_ranges", key, "t_max"), Full(f[8])) << key;
  }
}

TEST_F(EssviReportTool, TheTimingReportIsOneTimedSviEightTable) {
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
  EXPECT_EQ(timing.name, "essvi_fit_time");
  EXPECT_EQ(timing.targets, (std::vector<std::string>{"SVI-8"}));
  EXPECT_TRUE(timing.timing);
  EXPECT_TRUE(timing.uses_chain);
  ASSERT_EQ(timing.rows.size(), 2U);
  const auto cell = [&timing](const results::ReportRow& row, std::string_view column) {
    const auto index = timing.column(column);
    return index ? row.cells[*index] : std::string();
  };
  for (const char* key : {"fit", "fit_and_checks"}) {
    const results::ReportRow* row = timing.row(key);
    ASSERT_NE(row, nullptr) << key;
    const double median = results::read_real_cell(cell(*row, "median")).value_or(-1.0);
    const double least = results::read_real_cell(cell(*row, "min")).value_or(-1.0);
    EXPECT_GT(least, 0.0) << key;
    EXPECT_LE(least, median) << key;
    EXPECT_EQ(cell(*row, "repetitions"), "2") << key;
  }
}

TEST_F(EssviReportTool, RunFlagsAreForTheReportOnly) {
  const RunResult result = RunTool("--reportable " + Arguments());
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.output.find("usage: aadp_essvi_report"), std::string::npos) << result.output;
}

TEST_F(EssviReportTool, AReportableReportFromATreeThatIsNotCleanIsRefusedBeforeAnyWork) {
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
