// Runs the real aadp_quote_sensitivity_report binary on synthetic files made from hand-written
// SVI parameters, at reduced path counts. Nothing here comes from a TAIFEX file, and no number it
// prints is a result.

#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "../data/taifex_fixture.hpp"
#include "data/attribution.hpp"
#include "data/taifex_options.hpp"
#include "synthetic_chain.hpp"

namespace aadp {
namespace {

namespace fs = std::filesystem;

struct RunResult {
  std::string output;
  int status;
};

std::string ShellArg(const std::string& text) { return "'" + text + "'"; }

RunResult RunTool(const std::string& arguments) {
  const std::string command =
      ShellArg(AADP_QUOTE_SENSITIVITY_REPORT_PATH) + " " + arguments + " 2>&1";
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
    const std::vector<testing::SyntheticExpiry> surface = {
        {.expiry = "202411",
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
    WriteFile(dir_ / "options.csv",
              OptionsCsv(testing::SyntheticRows(surface, 20000.0, 25000.0, 100.0)));
    WriteFile(dir_ / "futures.csv", FuturesCsv());
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

}  // namespace
}  // namespace aadp
