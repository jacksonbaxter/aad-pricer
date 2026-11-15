// Runs the real aadp_quote_sensitivity_report binary on synthetic files made from hand-written
// SVI parameters, at reduced path counts. Nothing here comes from a TAIFEX file, and no number it
// prints is a result.

#include <gtest/gtest.h>
#include <sys/wait.h>  // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <cctype>
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
  const std::string command = ShellArg(AADP_QUOTE_SENSITIVITY_REPORT_PATH) + " " + arguments +
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

class QuoteSensitivityReportTool : public ::testing::Test {
 protected:
  void SetUp() override {
    // ctest runs each test as its own process, in parallel, so no two can share these files.
    dir_ = fs::path(AADP_QUOTE_SENSITIVITY_REPORT_TEST_SCRATCH) /
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
           ShellArg((dir_ / "futures.csv").string()) + " --threads 2";
  }

  [[nodiscard]] const fs::path& Dir() const { return dir_; }

 private:
  fs::path dir_;
};

// No refusal, and no nan or inf printed as a number: the word, after a space or sign and not
// followed by a letter (the attribution says "information").
void ExpectClean(const std::string& output) {
  EXPECT_EQ(output.find("REFUSED"), std::string::npos) << output;
  for (const std::string word : {"nan", "inf"}) {
    for (std::size_t at = output.find(word); at != std::string::npos;
         at = output.find(word, at + 1)) {
      const bool after_sign =
          at > 0 && (output[at - 1] == ' ' || output[at - 1] == '+' || output[at - 1] == '-');
      const bool word_ends =
          at + 3 == output.size() || std::isalpha(static_cast<unsigned char>(output[at + 3])) == 0;
      EXPECT_FALSE(after_sign && word_ends) << word << " at " << at << "\n"
                                            << output.substr(at > 200 ? at - 200 : 0, 400);
    }
  }
}

TEST_F(QuoteSensitivityReportTool, ReportCoversSlicesContractsAndSummaries) {
  const RunResult result = RunTool(Arguments() + " --paths 1024");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
  for (const char* expected : {"not tradable quotes",
                               "Maturity reaches 33 days only",
                               "ITERATION ONLY, not a result",
                               "run: seed 20261115, paths 1024",
                               "== slice 202411:",
                               "== slice 202411W4:",
                               "== slice 202412:",
                               "exact Hessian of Phi",
                               "Gauss-Newton 2 J^T J",
                               "exact - GN",
                               "KKT condition",
                               "-- slice 202412, every quote",
                               "d theta* / d(F, D, sigma_leg), exact",
                               "== asian-arithmetic-call ==",
                               "== down-out-call-discrete-one-step ==",
                               "== down-out-call-continuous-bridge ==",
                               "CRN difference",
                               "summary, market fixed, exact",
                               "summary, market fixed, Gauss-Newton",
                               "summary, through parity, exact",
                               "(every settlement up one point)"}) {
    EXPECT_NE(result.output.find(expected), std::string::npos) << expected << "\n" << result.output;
  }
  ExpectClean(result.output);
}

TEST_F(QuoteSensitivityReportTool, ShortCheckPassesAndSaysItIsShort) {
  const RunResult result = RunTool(Arguments() + " --mode check --paths 4096 --stride 16");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
  for (const char* expected : {"every 16th (short run, not the registered check) quote",
                               "== check summary ==", "parameters, market fixed, exact Hessian",
                               "prices, through parity, exact Hessian", "verdict: PASS"}) {
    EXPECT_NE(result.output.find(expected), std::string::npos) << expected << "\n" << result.output;
  }
  ExpectClean(result.output);
}

TEST_F(QuoteSensitivityReportTool, TimingIsLabelledIterationOnly) {
  const RunResult result = RunTool(Arguments() + " --mode timing --paths 1024 --repetitions 1");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
  EXPECT_NE(result.output.find("== timing: ITERATION ONLY, not a result =="), std::string::npos)
      << result.output;
}

TEST_F(QuoteSensitivityReportTool, MissingFileFailsAndSaysHowToGetIt) {
  const RunResult result = RunTool("--options " + ShellArg((Dir() / "absent.csv").string()) +
                                   " --futures " + ShellArg((Dir() / "futures.csv").string()));
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.output.find("scripts/fetch_data.sh"), std::string::npos) << result.output;
}

TEST_F(QuoteSensitivityReportTool, BadArgumentsAreAUsageError) {
  for (const char* bad : {"--paths 100", "--paths 0", "--mode fast", "--threads 0", "--stride 0",
                          "--repetitions 0", "--paths"}) {
    const RunResult result = RunTool(bad);
    EXPECT_NE(result.status, 0) << bad;
    EXPECT_NE(result.output.find("usage: aadp_quote_sensitivity_report"), std::string::npos)
        << bad << "\n"
        << result.output;
  }
}

#if AADP_REPORT_MODE

// The comma-separated fields of every machine-readable line that starts with `kind,`.
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
    out.push_back(std::move(fields));
  }
  return out;
}

double Full(const std::string& text) { return std::strtod(text.c_str(), nullptr); }

constexpr std::array<const char*, 5> kParameters = {"a", "b", "rho", "m", "sigma"};

// One mode's text and report on the same inputs, the report read back by every rule.
class QuoteSensitivityReport : public QuoteSensitivityReportTool {
 protected:
  void Run(const std::string& mode_arguments) {
    text_ = RunTool(Arguments() + " " + mode_arguments);
    report_ = RunTool("--report " + Arguments() + " " + mode_arguments, false);
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

  // Each real field of a line against its cell, from field `first` on, in the columns' order.
  void ExpectReals(std::string_view name, std::string_view row,
                   const std::vector<std::string>& fields, std::size_t first,
                   const std::vector<std::string>& columns) const {
    ASSERT_GE(fields.size(), first + columns.size()) << row;
    for (std::size_t c = 0; c < columns.size(); ++c) {
      EXPECT_EQ(Real(name, row, columns[c]), Full(fields[first + c]))
          << name << " " << row << "/" << columns[c];
    }
  }

  [[nodiscard]] std::vector<std::string> Names() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const results::ReportTable& table : tables_) names.push_back(table.name);
    return names;
  }

  void ExpectTheFileShape(std::string_view mode) const {
    EXPECT_TRUE(report_.output.starts_with("aadp-results 1\n")) << report_.output;
    EXPECT_TRUE(results::missing_keys(file_, results::kRunHeaderKeys).empty());
    EXPECT_TRUE(results::missing_keys(file_, results::kPerfHeaderKeys).empty());
    EXPECT_EQ(file_.value("claim"), "result");
    EXPECT_EQ(results::file_kind(file_), results::FileKind::kReport);
    EXPECT_EQ(file_.value("report_tool"),
              "aadp_quote_sensitivity_report --report --mode " + std::string(mode));
    EXPECT_NE(file_.required("report_inputs").find("not tradable quotes"), std::string_view::npos);
    EXPECT_NE(file_.required("report_inputs").find("33 days only"), std::string_view::npos);
    ASSERT_FALSE(file_.sections.empty());
    EXPECT_EQ(file_.sections.front().name, "attribution");
    EXPECT_EQ(file_.sections.front().body, std::string(attribution_text()));
    for (const results::ReportTable& table : tables_) {
      EXPECT_TRUE(table.uses_chain) << table.name;
      EXPECT_EQ(table.targets, (std::vector<std::string>{"SVI-10"})) << table.name;
    }
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

class QuoteSensitivitySummaryReport : public QuoteSensitivityReport {
 protected:
  void SetUp() override {
    QuoteSensitivityReport::SetUp();
    Run("--paths 1024");
    ASSERT_EQ(Text().status, 0) << Text().output;
  }

  // Every slice, gauss_newton and multiplier line of the text is a row of the slice tables.
  void ExpectSliceTablesAreTheTextsDoubles() const {
    const auto slices = Lines(Text().output, "slice");
    ASSERT_EQ(slices.size(), 3U);
    for (const auto& f : slices) {
      ASSERT_EQ(f.size(), 18U);
      EXPECT_EQ(Cell("qs_slices", f[1], "quotes"), f[2]);
      EXPECT_EQ(Cell("qs_slices", f[1], "on_tick_boundary"), f[4]);
      EXPECT_EQ(Cell("qs_slices", f[1], "active_bounds"), f[5]);
      EXPECT_EQ(Real("qs_slices", f[1], "objective"), Full(f[3]));
      ExpectReals("qs_slices", f[1], f, 6,
                  {"stationarity", "gradient_norm", "hessian_asymmetry", "hessian_gn_difference",
                   "condition", "manifold_residual", "newton_a", "newton_b", "newton_rho",
                   "newton_m", "newton_sigma"});
      EXPECT_EQ(Cell("qs_slices", f[1], "gauss_newton"), f[17]);
    }
    const auto gn = Lines(Text().output, "gauss_newton");
    EXPECT_EQ(Table("qs_gauss_newton").rows.size(), gn.size());
    for (const auto& f : gn) {
      ASSERT_EQ(f.size(), 9U);
      ExpectReals("qs_gauss_newton", f[1], f, 2,
                  {"condition", "manifold_residual", "against_exact_a", "against_exact_b",
                   "against_exact_rho", "against_exact_m", "against_exact_sigma"});
    }
    const auto multipliers = Lines(Text().output, "multiplier");
    if (!multipliers.empty()) {
      EXPECT_EQ(Table("qs_multipliers").rows.size(), multipliers.size());
    }
    for (const auto& f : multipliers) {
      ASSERT_EQ(f.size(), 9U);
      ExpectReals(
          "qs_multipliers", f[1] + "." + f[2], f, 3,
          {"newton", "least_squares", "second_step", "margin", "significance", "quote_distance"});
    }
  }
};

TEST_F(QuoteSensitivitySummaryReport, IsOneResultsFileOfSummaries) {
  ExpectTheFileShape("report");
  EXPECT_EQ(File().value("report_run"),
            "seed 20261115, paths 1024, batches 64, threads 2, 33 dates, n 4");
  // A table without rows is not written: on this surface no bound is active, so no multiplier.
  std::vector<std::string> expected = {"qs_slices", "qs_gauss_newton"};
  if (!Lines(Text().output, "multiplier").empty()) expected.emplace_back("qs_multipliers");
  for (const char* name :
       {"qs_price", "qs_market_derivatives", "qs_quote_buckets", "qs_quote_totals"}) {
    expected.emplace_back(name);
  }
  EXPECT_EQ(Names(), expected);
  for (const results::ReportTable& table : Tables()) EXPECT_FALSE(table.timing) << table.name;
}

// Summaries only: no row per quote, and no column for a strike, a settlement, a fitted
// parameter, a forward or a discount.
TEST_F(QuoteSensitivitySummaryReport, HoldsSummariesOnly) {
  EXPECT_EQ(Table("qs_slices").rows.size(), 3U);
  EXPECT_EQ(Table("qs_price").rows.size(), 3U);
  EXPECT_EQ(Table("qs_market_derivatives").rows.size(), 9U);
  EXPECT_EQ(Table("qs_quote_totals").rows.size(), 9U);
  // At most 6 buckets per expiry, version and contract.
  EXPECT_LE(Table("qs_quote_buckets").rows.size(), 3U * 3U * 3U * 6U);
  for (const results::ReportTable& table : Tables()) {
    for (const results::ReportColumn& column : table.columns) {
      for (const char* forbidden : {"strike", "settlement", "forward", "discount", "theta"}) {
        EXPECT_FALSE(column.name.contains(forbidden)) << table.name << "." << column.name;
      }
    }
  }
}

TEST_F(QuoteSensitivitySummaryReport, TheSliceTablesAreTheTextsDoubles) {
  ExpectSliceTablesAreTheTextsDoubles();
}

TEST_F(QuoteSensitivitySummaryReport, ThePriceTablesAreTheTextsDoubles) {
  const auto prices = Lines(Text().output, "price");
  ASSERT_EQ(prices.size(), 3U);
  for (const auto& f : prices) {
    ASSERT_EQ(f.size(), 5U);
    EXPECT_EQ(Cell("qs_price", f[1], "paths"), f[2]);
    ExpectReals("qs_price", f[1], f, 3, {"price", "price_se"});
  }
  const auto market = Lines(Text().output, "market");
  ASSERT_EQ(market.size(), 9U);
  for (const auto& f : market) {
    ASSERT_EQ(f.size(), 13U);
    const std::string key = f[1] + "." + f[2];
    ExpectReals("qs_market_derivatives", key, f, 3, {"dp_df", "dp_df_se"});
    EXPECT_EQ(Cell("qs_market_derivatives", key, "dp_df_level"), f[5]);
    ExpectReals("qs_market_derivatives", key, f, 6,
                {"dp_df_truncation", "dp_df_rounding", "dp_dd", "dp_dd_se"});
    EXPECT_EQ(Cell("qs_market_derivatives", key, "dp_dd_level"), f[10]);
    ExpectReals("qs_market_derivatives", key, f, 11, {"dp_dd_truncation", "dp_dd_rounding"});
  }
  const auto buckets = Lines(Text().output, "bucket");
  ASSERT_FALSE(buckets.empty());
  EXPECT_EQ(Table("qs_quote_buckets").rows.size(), buckets.size());
  const std::array<const char*, 6> from = {"open", "-0.10", "-0.05", "-0.02", "0.02", "0.05"};
  const std::array<const char*, 6> to = {"-0.10", "-0.05", "-0.02", "0.02", "0.05", "open"};
  for (const auto& f : buckets) {
    ASSERT_EQ(f.size(), 9U);
    const std::string key = f[1] + "." + f[2] + "." + f[3] + "." + f[4];
    EXPECT_EQ(Cell("qs_quote_buckets", key, "quotes"), f[5]);
    const std::size_t b = std::stoul(f[4].substr(1));
    ASSERT_LT(b, from.size()) << key;
    EXPECT_EQ(Cell("qs_quote_buckets", key, "k_from"), from[b]) << key;
    EXPECT_EQ(Cell("qs_quote_buckets", key, "k_to"), to[b]) << key;
    ExpectReals("qs_quote_buckets", key, f, 6, {"sum", "sum_se", "max_abs"});
  }
  const auto totals = Lines(Text().output, "total");
  ASSERT_EQ(totals.size(), 9U);
  for (const auto& f : totals) {
    ASSERT_EQ(f.size(), 6U);
    const std::string key = f[1] + "." + f[2];
    EXPECT_EQ(Cell("qs_quote_totals", key, "quotes"), f[3]);
    ExpectReals("qs_quote_totals", key, f, 4, {"sum", "sum_se"});
  }
}

// 202412 steeper on the right than the fit's slope bound allows, b (1 + |rho|) = 4.18 > 4, so
// the bound is active there and the slice has a multiplier.
class QuoteSensitivityBoundReport : public QuoteSensitivitySummaryReport {
 protected:
  [[nodiscard]] std::vector<testing::SyntheticExpiry> Surface() const override {
    std::vector<testing::SyntheticExpiry> surface = QuoteSensitivitySummaryReport::Surface();
    surface[2].params = {.a = 1e-4, .b = 2.2, .rho = 0.9, .m = 0.15, .sigma = 0.05};
    return surface;
  }
};

TEST_F(QuoteSensitivityBoundReport, TheMultipliersAreTheTextsDoubles) {
  ASSERT_FALSE(Lines(Text().output, "multiplier").empty()) << Text().output;
  ExpectSliceTablesAreTheTextsDoubles();
}

class QuoteSensitivityCheckReport : public QuoteSensitivityReport {
 protected:
  void SetUp() override {
    QuoteSensitivityReport::SetUp();
    Run("--mode check --paths 4096 --stride 32");
  }
};

TEST_F(QuoteSensitivityCheckReport, IsOneResultsFileWithTheVerdictInACell) {
  ExpectTheFileShape("check");
  EXPECT_EQ(File().value("report_run"),
            "seed 20261115, paths 4096, batches 64, threads 2, 33 dates, n 4, stride 32, refits "
            "polished");
  EXPECT_EQ(Names(),
            (std::vector<std::string>{"qs_check_run", "qs_check_refit_error", "qs_check_tallies",
                                      "qs_check_price_failures", "qs_check_verdict"}));
  for (const results::ReportTable& table : Tables()) EXPECT_FALSE(table.timing) << table.name;
  // The verdict is the text's, and the text keeps its exit code: 0 on PASS, 1 on FAIL.
  const std::string verdict = Cell("qs_check_verdict", "verdict", "verdict");
  EXPECT_NE(Text().output.find("  verdict: " + verdict + "\n"), std::string::npos) << verdict;
  EXPECT_EQ(WEXITSTATUS(Text().status), verdict == "PASS" ? 0 : 1) << Text().output;
  EXPECT_EQ(verdict == "PASS", Cell("qs_check_verdict", "verdict", "failed") == "0");
}

TEST_F(QuoteSensitivityCheckReport, TheCheckTablesHoldTheTextsCounts) {
  const auto run = Lines(Text().output, "check");
  ASSERT_EQ(run.size(), 1U);
  ASSERT_EQ(run[0].size(), 5U);
  EXPECT_EQ(Cell("qs_check_run", "check", "stride"), run[0][1]);
  EXPECT_EQ(Cell("qs_check_run", "check", "refit"), run[0][2]);
  EXPECT_EQ(Cell("qs_check_run", "check", "levels"), run[0][3]);
  EXPECT_EQ(Cell("qs_check_run", "check", "unusable_levels"), run[0][4]);
  const auto refit = Lines(Text().output, "refit_error");
  ASSERT_EQ(refit.size(), 3U);
  for (const auto& f : refit) {
    ExpectReals("qs_check_refit_error", f[1], f, 2,
                std::vector<std::string>(kParameters.begin(), kParameters.end()));
  }
  const std::vector<std::string> counts = {"comparisons", "passed", "unresolved", "failed"};
  const auto tallies = Lines(Text().output, "tally");
  ASSERT_EQ(tallies.size(), 9U);
  for (const auto& f : tallies) {
    ASSERT_EQ(f.size(), 8U);
    EXPECT_EQ(Cell("qs_check_tallies", f[1], "role"), f[2]);
    for (std::size_t c = 0; c < counts.size(); ++c) {
      EXPECT_EQ(Cell("qs_check_tallies", f[1], counts[c]), f[3 + c]) << f[1] << " " << counts[c];
    }
    EXPECT_EQ(std::stoul(f[6]), std::stoul(f[3]) - std::stoul(f[4])) << f[1];
    ExpectReals("qs_check_tallies", f[1], f, 7, {"worst_ratio"});
  }
  // Gauss-Newton fails comparisons here, as registered, and the report keeps each failure.
  std::size_t disclosure_failed = 0;
  for (const auto& f : tallies) {
    if (f[2] == "disclosure") disclosure_failed += std::stoul(f[6]);
  }
  EXPECT_GT(disclosure_failed, 0U) << Text().output;
  const auto by_contract = Lines(Text().output, "contract_tally");
  ASSERT_EQ(by_contract.size(), 6U);
  std::size_t price_comparisons = 0;
  std::size_t price_failed = 0;
  for (const auto& f : by_contract) {
    ASSERT_EQ(f.size(), 8U);
    const std::string key = f[1] + "." + f[2];
    for (std::size_t c = 0; c < counts.size(); ++c) {
      EXPECT_EQ(Cell("qs_check_price_failures", key, counts[c]), f[3 + c]) << key;
    }
    ExpectReals("qs_check_price_failures", key, f, 7, {"worst_ratio"});
    price_comparisons += std::stoul(f[3]);
    price_failed += std::stoul(f[6]);
    // Each quote of a chain is judged on all three contracts.
    for (const auto& tally : tallies) {
      if (tally[1] == "prices." + f[2] + ".exact") {
        EXPECT_EQ(3 * std::stoul(f[3]), std::stoul(tally[3])) << key;
      }
    }
  }
  const auto verdict = Lines(Text().output, "verdict");
  ASSERT_EQ(verdict.size(), 1U);
  ASSERT_EQ(verdict[0].size(), 6U);
  EXPECT_EQ(Cell("qs_check_verdict", "verdict", "verdict"), verdict[0][1]);
  EXPECT_EQ(Cell("qs_check_verdict", "verdict", "comparisons"), verdict[0][2]);
  EXPECT_EQ(Cell("qs_check_verdict", "verdict", "failed"), verdict[0][3]);
  EXPECT_EQ(Cell("qs_check_verdict", "verdict", "price_comparisons"), verdict[0][4]);
  EXPECT_EQ(Cell("qs_check_verdict", "verdict", "price_failed"), verdict[0][5]);
  // By contract, the price comparisons add up to the chains' tallies.
  EXPECT_EQ(std::to_string(price_comparisons), verdict[0][4]);
  EXPECT_EQ(std::to_string(price_failed), verdict[0][5]);
}

// The bound surface with the production refits: the registered check fails here, a price
// comparison of the discrete contract outside its tolerance (error over tolerance about 1.1).
class QuoteSensitivityFailedCheckReport : public QuoteSensitivityReport {
 protected:
  [[nodiscard]] std::vector<testing::SyntheticExpiry> Surface() const override {
    std::vector<testing::SyntheticExpiry> surface = QuoteSensitivityReport::Surface();
    surface[2].params = {.a = 1e-4, .b = 2.2, .rho = 0.9, .m = 0.15, .sigma = 0.05};
    return surface;
  }
  void SetUp() override {
    QuoteSensitivityReport::SetUp();
    Run("--mode check --paths 4096 --stride 32 --refit production");
  }
};

TEST_F(QuoteSensitivityFailedCheckReport, AFailedCheckStaysAFailureWithItsCount) {
  EXPECT_EQ(File().value("report_run"),
            "seed 20261115, paths 4096, batches 64, threads 2, 33 dates, n 4, stride 32, refits "
            "production");
  const auto verdict = Lines(Text().output, "verdict");
  ASSERT_EQ(verdict.size(), 1U);
  ASSERT_EQ(verdict[0].size(), 6U);
  ASSERT_EQ(verdict[0][1], "FAIL") << Text().output;
  // The text keeps its exit code; the report is written and says FAIL in a cell, with the counts.
  EXPECT_EQ(WEXITSTATUS(Text().status), 1);
  EXPECT_EQ(Cell("qs_check_verdict", "verdict", "verdict"), "FAIL");
  EXPECT_EQ(Cell("qs_check_verdict", "verdict", "failed"), verdict[0][3]);
  EXPECT_EQ(Cell("qs_check_verdict", "verdict", "price_failed"), verdict[0][5]);
  EXPECT_GT(std::stoul(verdict[0][3]), 0U);
  const std::vector<std::string> counts = {"comparisons", "passed", "unresolved", "failed"};
  std::size_t required_failed = 0;
  for (const auto& f : Lines(Text().output, "tally")) {
    ASSERT_EQ(f.size(), 8U);
    for (std::size_t c = 0; c < counts.size(); ++c) {
      EXPECT_EQ(Cell("qs_check_tallies", f[1], counts[c]), f[3 + c]) << f[1] << " " << counts[c];
    }
    if (f[2] == "required") required_failed += std::stoul(f[6]);
  }
  EXPECT_EQ(std::to_string(required_failed), verdict[0][3]);
  std::size_t price_failed = 0;
  for (const auto& f : Lines(Text().output, "contract_tally")) {
    ASSERT_EQ(f.size(), 8U);
    EXPECT_EQ(Cell("qs_check_price_failures", f[1] + "." + f[2], "failed"), f[6]);
    price_failed += std::stoul(f[6]);
  }
  EXPECT_EQ(std::to_string(price_failed), verdict[0][5]);
}

TEST_F(QuoteSensitivityReportTool, TheTimingReportIsOneTimedSviTenTable) {
  const RunResult result =
      RunTool("--report --mode timing --paths 1024 --repetitions 2 " + Arguments(), false);
  ASSERT_EQ(result.status, 0) << result.output;
  auto file = results::parse_results_file(result.output);
  ASSERT_TRUE(file.has_value()) << file.error();
  EXPECT_EQ(file->value("repetitions"), "2");
  EXPECT_EQ(file->sections.front().body, std::string(attribution_text()));
  const auto tables = results::read_report(*file);
  ASSERT_TRUE(tables.has_value()) << tables.error().detail;
  ASSERT_EQ(tables->size(), 1U);
  const results::ReportTable& timing = tables->front();
  EXPECT_EQ(timing.name, "qs_time");
  EXPECT_EQ(timing.targets, (std::vector<std::string>{"SVI-10"}));
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
    const double grad = cell("gradient_median");
    const double ift = cell("implicit_median");
    const double parity = cell("parity_median");
    EXPECT_GT(grad, 0.0) << row.key;
    EXPECT_GE(ift, 0.0) << row.key;
    EXPECT_GT(parity, 0.0) << row.key;
    EXPECT_EQ(cell("implicit_ratio"), (grad + ift) / grad) << row.key;
    EXPECT_EQ(cell("parity_ratio"), (grad + ift + parity) / grad) << row.key;
    EXPECT_EQ(text("repetitions"), "2") << row.key;
  }
}

TEST_F(QuoteSensitivityReportTool, RunFlagsAreForTheReportOnly) {
  // At few paths, so that a tool which took the flag would finish and say so.
  const RunResult result = RunTool("--reportable " + Arguments() + " --paths 1024");
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.output.find("usage: aadp_quote_sensitivity_report"), std::string::npos)
      << result.output;
}

TEST_F(QuoteSensitivityReportTool, AReportableReportFromATreeThatIsNotCleanIsRefusedBeforeAnyWork) {
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
