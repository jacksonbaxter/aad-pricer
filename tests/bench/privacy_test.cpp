#include "results/privacy.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "fixtures.hpp"

namespace aadp::results {
namespace {

std::string kinds(std::string_view text, const PrivacyConfig& config = {}) {
  std::string out;
  for (const PrivacyHit& hit : scan_private_text(text, config)) {
    out += std::string(hit.kind) + "@" + std::to_string(hit.line) + ";";
  }
  return out;
}

TEST(Privacy, FindsHomePaths) {
  EXPECT_EQ(kinds(testing::home_path("x")), "home path@1;");
  EXPECT_EQ(kinds("ok\n/" + std::string("Users") + "/bob/x"), "home path@2;");
  EXPECT_EQ(kinds("/" + std::string("root") + "/x"), "home path@1;");
  EXPECT_EQ(kinds("cache=~/x"), "home path@1;");
  EXPECT_EQ(kinds("C:\\Users\\bob"), "home path@1;");
}

TEST(Privacy, FindsAddresses) {
  EXPECT_EQ(kinds("from 203.0.113.7:22"), "ip address@1;");
  EXPECT_EQ(kinds("addr=10.1.2.3"), "ip address@1;");
  EXPECT_EQ(kinds("host=10.1.2.3"), "ip address@1;hostname@1;");
  EXPECT_EQ(kinds("fd7a:115c:a1e0::1234"), "ip address@1;");
  EXPECT_EQ(kinds("2001:db8:1:2:3:4:5:6"), "ip address@1;");
}

TEST(Privacy, LeavesVersionsTimesAndNamesAlone) {
  for (const std::string_view text :
       {"Clang 23.1.4", "Linux 6.12.113+deb13-amd64", "1.2.3.4.5", "999.1.1.1",
        "2026-01-02T03:04:05Z", "03:04:05", "aadp::bench::run", "std::vector",
        "harness_selftest/iterations:2000", "cpu4:3500000",
        "-ffp-contract=off -DHWY_COMPILE_ONLY_STATIC -O3 -DNDEBUG", "0.123 4.567",
        "1.2340000000000000e+00"}) {
    EXPECT_EQ(kinds(text), "") << text;
  }
}

TEST(Privacy, FindsHostnamesIgnoringCaseAndDomain) {
  PrivacyConfig config;
  config.hostnames = {"My-Box.local"};
  EXPECT_EQ(kinds("x my-box y", config), "hostname@1;");
  EXPECT_EQ(kinds("MY-BOX.LOCAL", config), "hostname@1;");
  EXPECT_EQ(kinds("other", config), "");
}

TEST(Privacy, FindsAHostFieldWhateverItsValue) {
  EXPECT_EQ(kinds("copied from host=elsewhere"), "hostname@1;");
  EXPECT_EQ(kinds("{\"host=_name\": 1}"), "hostname@1;");
}

TEST(Privacy, FindsGoogleBenchmarkKeysThatEscapedTheFilter) {
  EXPECT_EQ(kinds("{\"host_name\": \"x\"}"), "google benchmark private key@1;");
  EXPECT_EQ(kinds("{\"executable\": \"./bench\"}"), "google benchmark private key@1;");
  EXPECT_EQ(kinds("not executable here"), "");
}

TEST(Privacy, FindsForbiddenStrings) {
  PrivacyConfig config;
  config.forbidden = {"alice", ""};
  EXPECT_EQ(kinds("owner Alice", config), "forbidden string@1;");
  EXPECT_EQ(kinds("nobody", config), "");
}

}  // namespace
}  // namespace aadp::results
