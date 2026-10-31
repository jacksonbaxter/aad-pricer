// Runs the real chain_profile binary on synthetic files. A unit test of the helper would pass even
// if the tool stopped calling it.

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>

#include "data/attribution.hpp"
#include "taifex_fixture.hpp"

namespace aadp {
namespace {

namespace fs = std::filesystem;

struct RunResult {
  std::string output;
  int status;
};

std::string shell_arg(const std::string& text) { return "'" + text + "'"; }

RunResult run(const std::string& arguments) {
  const std::string command = shell_arg(AADP_CHAIN_PROFILE_PATH) + " " + arguments + " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  EXPECT_NE(pipe, nullptr);
  RunResult result{.output = "", .status = -1};
  if (pipe == nullptr) {
    return result;
  }
  std::array<char, 4096> buffer{};
  std::size_t count = 0;
  while ((count = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
    result.output.append(buffer.data(), count);
  }
  result.status = pclose(pipe);
  return result;
}

void write_file(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(out.good()) << path;
}

class ChainProfile : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = AADP_DATA_TEST_SCRATCH;
    fs::create_directories(dir_);
    auto option = testing::option_fields();
    option[testing::kOptContract] = "TXO";
    write_file(dir_ / "options.csv",
               testing::kOptionsHeader + "\r\n" + testing::join(option) + "\r\n");
    write_file(dir_ / "futures.csv", testing::kFuturesHeader + "\r\n" +
                                         testing::join(testing::futures_fields()) + "\r\n");
  }

  [[nodiscard]] std::string arguments() const {
    return "--options " + shell_arg((dir_ / "options.csv").string()) + " --futures " +
           shell_arg((dir_ / "futures.csv").string());
  }

  [[nodiscard]] const fs::path& dir() const { return dir_; }

 private:
  fs::path dir_;
};

TEST_F(ChainProfile, OutputCarriesTheFullAttribution) {
  const RunResult result = run(arguments());
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_NE(result.output.find(attribution_text()), std::string::npos) << result.output;
}

TEST_F(ChainProfile, AttributionComesBeforeAnyCount) {
  const RunResult result = run(arguments());
  EXPECT_EQ(result.output.find(attribution_text()), 0U) << result.output;
}

TEST_F(ChainProfile, PrintsCountsThroughTheReaders) {
  const RunResult result = run(arguments());
  EXPECT_NE(result.output.find("options rows: 1"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("trade date 20240103: 1"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("TXO rows: 1"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("futures rows: 1"), std::string::npos) << result.output;
}

TEST_F(ChainProfile, MissingFileFailsAndSaysHowToGetIt) {
  const RunResult result = run("--options " + shell_arg((dir() / "no-such-file.csv").string()) +
                               " --futures " + shell_arg((dir() / "futures.csv").string()));
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.output.find("scripts/fetch_data.sh"), std::string::npos) << result.output;
}

TEST_F(ChainProfile, WrongHeaderFails) {
  write_file(dir() / "bad.csv", "not,a,header\r\n");
  const RunResult result = run("--options " + shell_arg((dir() / "bad.csv").string()) +
                               " --futures " + shell_arg((dir() / "futures.csv").string()));
  EXPECT_NE(result.status, 0);
  EXPECT_NE(result.output.find("header mismatch"), std::string::npos) << result.output;
}

}  // namespace
}  // namespace aadp
