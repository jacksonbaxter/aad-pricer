// Runs the real aadp_essvi_report binary on synthetic files made from hand-written eSSVI
// parameters (docs/NOTES.md, "Stage 7c": H, converted to raw SVI), at reduced path counts. Nothing
// here comes from a TAIFEX file, and no number it prints is a result.

#include <gtest/gtest.h>

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

namespace aadp {
namespace {

namespace fs = std::filesystem;

struct RunResult {
  std::string output;
  int status;
};

std::string ShellArg(const std::string& text) { return "'" + text + "'"; }

RunResult RunTool(const std::string& arguments) {
  const std::string command = ShellArg(AADP_ESSVI_REPORT_PATH) + " " + arguments + " 2>&1";
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
    // H, the pre-registered hand-made surface, as raw SVI.
    const std::vector<testing::SyntheticExpiry> surface = {
        {.expiry = "202411",
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
         .params =
             svi::ToRawSvi(svi::EssviSlice<double>{.theta = 2.8e-3, .rho = -0.5, .psi = 0.05})}};
    WriteFile(dir_ / "options.csv",
              OptionsCsv(testing::SyntheticRows(surface, 20000.0, 25000.0, 100.0)));
    WriteFile(dir_ / "futures.csv", FuturesCsv());
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

}  // namespace
}  // namespace aadp
