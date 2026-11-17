// Runs the real aadp_run_oracles binary, at smoke size, on a synthetic chain made from
// hand-written SVI parameters. Nothing here comes from a TAIFEX file.

#include <gtest/gtest.h>
#include <sys/wait.h>  // IWYU pragma: keep

#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

#include "../data/taifex_fixture.hpp"
#include "../svi/synthetic_chain.hpp"
#include "data/attribution.hpp"
#include "data/taifex_options.hpp"

namespace aadp {
namespace {

namespace fs = std::filesystem;

struct RunResult {
  std::string output;
  int status = 0;
};

std::string ShellArg(const std::string& text) { return "'" + text + "'"; }

RunResult RunTool(const std::string& arguments) {
  const std::string command = ShellArg(AADP_RUN_ORACLES_PATH) + " " + arguments + " 2>&1";
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

// The plan and references are the committed ones; the chain is synthetic, in the file names
// LoadChainSurface reads.
class RunOraclesTool : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::path(AADP_RUN_ORACLES_TEST_SCRATCH) /
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
    WriteFile(dir_ / "DailyMarketReportOpt.csv",
              OptionsCsv(testing::SyntheticRows(surface, 20000.0, 25000.0, 100.0)));
    WriteFile(dir_ / "DailyMarketReportFut.csv", FuturesCsv());
  }

  static constexpr std::string_view kProduction =
      "run --estimator production --plan references/comparison_plan.csv --references "
      "references/oracle_values.csv --smoke --threads 2";

  [[nodiscard]] std::string Data() const { return " --data " + ShellArg(dir_.string()); }

 private:
  fs::path dir_;
};

// The surface rows are priced off the fitted chain, so the output opens with the whole
// attribution, before any line the chain could have shaped.
TEST_F(RunOraclesTool, AProductionRunOnTheChainOpensWithTheWholeAttribution) {
  const RunResult result = RunTool(std::string(kProduction) + Data());
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
  EXPECT_EQ(result.output.find(attribution_text(), 1), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("P3.AS."), std::string::npos) << result.output;
  EXPECT_EQ(result.output.find("NO DATA"), std::string::npos) << result.output;
}

// scripts/compare_measured.py rebuilds run 2's rows from this output by the row shape, so the
// attribution must hold no line of that shape.
TEST_F(RunOraclesTool, TheAttributionHoldsNoResultRow) {
  EXPECT_EQ(attribution_text().find(" estimate="), std::string::npos);
  EXPECT_EQ(attribution_text().find(" condition="), std::string::npos);
}

// Without the chain nothing is derived from it, so there is no attribution, and the surface
// rows are skipped, which fails the run.
TEST_F(RunOraclesTool, AProductionRunWithoutTheChainCarriesNoAttribution) {
  const RunResult result = RunTool(std::string(kProduction));
  EXPECT_NE(result.status, 0) << result.output;
  EXPECT_EQ(result.output.find("NO DATA"), 0U) << result.output;
  EXPECT_EQ(result.output.find(attribution_text()), std::string::npos) << result.output;
}

}  // namespace
}  // namespace aadp
