// The per-variant RSS probe. The two runs of the real binary check what went wrong on linux-zen1
// in stage 3b, where every child read the parent's peak: an idle child must not inherit a large
// parent's peak, and a variant that builds a tape must peak above an idle child.
#include "rss_probe.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aadp::bench {
namespace {

TEST(RssProbe, ReadsTheLastReportLine) {
  EXPECT_EQ(ParsePeakReport("aadp-rss-probe-peak-bytes 4096\n").value_or(0), 4096U);
  EXPECT_EQ(ParsePeakReport("noise\naadp-rss-probe-peak-bytes 1\naadp-rss-probe-peak-bytes 7\n")
                .value_or(0),
            7U);
  EXPECT_EQ(ParsePeakReport("aadp-rss-probe-peak-bytes 12").value_or(0), 12U);
}

TEST(RssProbe, RefusesAMissingOrBadReport) {
  for (const std::string output :
       {"", "noise\n", "aadp-rss-probe-peak-bytes \n", "aadp-rss-probe-peak-bytes 0\n",
        "aadp-rss-probe-peak-bytes 12x\n", "xaadp-rss-probe-peak-bytes 12\n"}) {
    EXPECT_FALSE(ParsePeakReport(output).has_value()) << output;
  }
}

// The bytes of a real line from debian: a tab, then spaces. Space-only padding was all the first
// parser took, so on Linux every probe failed.
TEST(RssProbe, ReadsVmHwmAsTheKernelWritesIt) {
  EXPECT_EQ(ParseVmHwm("Name:\tx\nVmPeak:\t   10000 kB\nVmHWM:\t    2304 kB\nVmRSS:\t    2000 kB\n")
                .value_or(0),
            2304U * 1024U);
  EXPECT_EQ(ParseVmHwm("VmHWM:  7 kB").value_or(0), 7U * 1024U);
  for (const std::string status : {"", "VmRSS:\t 1 kB\n", "VmHWM:\t\n", "VmHWM:\t 12 MB\n",
                                   "VmHWM:\t 0 kB\n", "VmHWM:\t x kB\n"}) {
    EXPECT_FALSE(ParseVmHwm(status).has_value()) << status;
  }
}

#ifdef AADP_BENCH_AAD_BIN
constexpr std::uint64_t kMiB = std::uint64_t{1024} * 1024;

TEST(RssProbe, AnIdleChildDoesNotInheritTheParentsPeak) {
  // 256 MiB, every page written, so this process's peak is at least that.
  std::vector<unsigned char> big(256 * kMiB);
  for (std::size_t i = 0; i < big.size(); i += 4096) big[i] = static_cast<unsigned char>(i >> 12);
  // The stores must happen: an empty asm that may read the buffer keeps them.
  asm volatile("" : : "r"(big.data()) : "memory");
  const auto idle = ChildPeakRss(AADP_BENCH_AAD_BIN, {"--aadp-rss-probe=idle"});
  ASSERT_TRUE(idle.has_value()) << idle.error();
  EXPECT_LT(*idle, 64 * kMiB) << "the child reports the parent's peak";
  EXPECT_GT(*idle, 256U * 1024U) << "a running process holds more than 256 KiB";
}

TEST(RssProbe, AVariantThatBuildsATapePeaksAboveIdle) {
  const auto idle = ChildPeakRss(AADP_BENCH_AAD_BIN, {"--aadp-rss-probe=idle"});
  ASSERT_TRUE(idle.has_value()) << idle.error();
  const auto tape = ChildPeakRss(AADP_BENCH_AAD_BIN, {"--aadp-rss-probe=mc-asian252.tape",
                                                      "--aadp-scale=500", "--aadp-threads=1"});
  ASSERT_TRUE(tape.has_value()) << tape.error();
  EXPECT_GT(*tape, *idle);
}

TEST(RssProbe, AProbeThatFailsIsAnError) {
  EXPECT_FALSE(ChildPeakRss(AADP_BENCH_AAD_BIN, {"--aadp-rss-probe=no-such.tape"}).has_value());
}
#endif

}  // namespace
}  // namespace aadp::bench
