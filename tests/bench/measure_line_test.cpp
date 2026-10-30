#include "results/measure_line.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "fixtures.hpp"

namespace aadp::results {
namespace {

TEST(MeasureLine, ParsesMacStartLine) {
  const auto line =
      parse_measure_line("measure.sh: start load1=2.15 host=Some-Mac at=2026-10-30T15:00:00Z");
  ASSERT_TRUE(line.has_value()) << line.error();
  EXPECT_EQ(line->kind, MeasureKind::kStart);
  EXPECT_EQ(line->load1, "2.15");
  EXPECT_DOUBLE_EQ(line->load1_value, 2.15);
  EXPECT_EQ(line->host, "Some-Mac");
  EXPECT_EQ(line->at, "2026-10-30T15:00:00Z");
  EXPECT_FALSE(line->exit_status.has_value());
  EXPECT_FALSE(line->governor.has_value());
}

TEST(MeasureLine, ParsesMacEndLine) {
  const auto line = parse_measure_line("measure.sh: end load1=2.20 exit=3 at=2026-10-30T15:00:09Z");
  ASSERT_TRUE(line.has_value()) << line.error();
  EXPECT_EQ(line->kind, MeasureKind::kEnd);
  EXPECT_EQ(line->exit_status, 3);
}

TEST(MeasureLine, ParsesLinuxFields) {
  const auto line = parse_measure_line(testing::start_line());
  ASSERT_TRUE(line.has_value()) << line.error();
  EXPECT_EQ(line->cpus, "4");
  EXPECT_EQ(line->governor, "performance");
  EXPECT_EQ(line->freq_khz, "cpu4:3500000");
  EXPECT_EQ(line->perf_event_paranoid, "2");
}

TEST(MeasureLine, AcceptsWhatTheLinuxScriptPrintsWhenAFileIsMissing) {
  const auto line = parse_measure_line(
      "measure.sh: end load1=0.1 exit=0 at=2026-10-30T15:00:09Z cpus=0-7,9 "
      "governor=performance,schedutil freq_khz=cpu0:1000,cpu1:unavailable perf_event_paranoid=");
  ASSERT_TRUE(line.has_value()) << line.error();
  EXPECT_EQ(line->cpus, "0-7,9");
  EXPECT_EQ(line->freq_khz, "cpu0:1000,cpu1:unavailable");
  EXPECT_FALSE(line->perf_event_paranoid.has_value());
}

TEST(MeasureLine, ToleratesCarriageReturnAndDoubleSpaces) {
  EXPECT_TRUE(parse_measure_line("measure.sh: end  load1=0.1  exit=0 at=2026-10-30T15:00:09Z\r")
                  .has_value());
}

TEST(MeasureLine, RefusesLoadThatIsNotAPlainDecimal) {
  for (const std::string& load :
       std::vector<std::string>{"", "abc", "1,23", "-1", "+1", "1.", ".5", "1e3", "inf", "nan",
                                "0x10", "1.2.3", std::string(33, '1')}) {
    const auto line =
        parse_measure_line("measure.sh: start load1=" + load + " host=a at=2026-10-30T15:00:00Z");
    EXPECT_FALSE(line.has_value()) << "load1=" << load;
  }
}

TEST(MeasureLine, RefusesMissingAndRepeatedFields) {
  const std::string at = "at=2026-10-30T15:00:00Z";
  EXPECT_FALSE(parse_measure_line("measure.sh: start host=a " + at).has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: start load1=1 host=a").has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: end load1=1 " + at).has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: start load1=1 load1=2 " + at).has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: start load1=1 " + at + " " + at).has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: start load1=1 host=a host=b " + at).has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: start").has_value());
}

TEST(MeasureLine, RefusesBadExitStatus) {
  const std::string at = " at=2026-10-30T15:00:00Z";
  for (const std::string status : {"", "-1", "256", "1000", "x", "0x0", "1 2"}) {
    EXPECT_FALSE(parse_measure_line("measure.sh: end load1=1 exit=" + status + at).has_value())
        << "exit=" << status;
  }
  EXPECT_TRUE(parse_measure_line("measure.sh: end load1=1 exit=255" + at).has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: start load1=1 exit=0" + at).has_value());
}

TEST(MeasureLine, RefusesBadTimestamps) {
  for (const std::string at :
       {"", "2026-10-30", "2026-13-30T15:00:00Z", "2026-10-32T15:00:00Z", "2026-10-30T25:00:00Z",
        "2026-10-30T15:61:00Z", "2026-10-30T15:00:00+01:00", "2026-10-30 15:00:00Z"}) {
    EXPECT_FALSE(parse_measure_line("measure.sh: start load1=1 at=" + at).has_value()) << at;
  }
}

TEST(MeasureLine, RefusesValuesOutsideTheirAlphabet) {
  const std::string at = " at=2026-10-30T15:00:00Z";
  EXPECT_FALSE(parse_measure_line("measure.sh: start load1=1 host=a/b" + at).has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: start load1=1 host=" + at).has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: start load1=1 cpus=a" + at).has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: start load1=1 governor=x/y" + at).has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: start load1=1 freq_khz=x/y" + at).has_value());
  EXPECT_FALSE(
      parse_measure_line("measure.sh: start load1=1 perf_event_paranoid=a" + at).has_value());
}

TEST(MeasureLine, RefusesLinesThatAreNotStartOrEnd) {
  EXPECT_FALSE(parse_measure_line("").has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: starting load1=1").has_value());
  EXPECT_FALSE(parse_measure_line("measure.sh: restored governor=schedutil").has_value());
  EXPECT_FALSE(parse_measure_line("  measure.sh: start load1=1").has_value());
}

TEST(MeasureLine, SanitizedLineHasNoHostAndParsesBack) {
  const auto line = parse_measure_line(testing::start_line());
  ASSERT_TRUE(line.has_value());
  const std::string clean = sanitized_measure_line(*line);
  EXPECT_EQ(clean.find("host"), std::string::npos);
  EXPECT_EQ(clean.find(testing::kHost), std::string::npos);
  EXPECT_NE(clean.find("load1=0.31"), std::string::npos);
  EXPECT_NE(clean.find("governor=performance"), std::string::npos);
  EXPECT_NE(clean.find("freq_khz=cpu4:3500000"), std::string::npos);
  EXPECT_NE(clean.find("at=2026-01-02T03:04:05Z"), std::string::npos);
  const auto again = parse_measure_line(clean);
  ASSERT_TRUE(again.has_value()) << again.error();
  EXPECT_EQ(sanitized_measure_line(*again), clean);
}

TEST(MeasureLine, SanitizedLineDropsFieldsItDoesNotKnow) {
  const auto line = parse_measure_line(
      "measure.sh: end load1=1 exit=0 at=2026-10-30T15:00:00Z user=somebody ip=203.0.113.7");
  ASSERT_TRUE(line.has_value()) << line.error();
  const std::string clean = sanitized_measure_line(*line);
  EXPECT_EQ(clean.find("somebody"), std::string::npos);
  EXPECT_EQ(clean.find("203.0.113.7"), std::string::npos);
}

TEST(MeasureScan, SortsTheLinesOfACapture) {
  const std::string capture = "noise\n" + testing::start_line() +
                              "\nbenchmark says hello\nmeasure.sh: start load1=oops\n" +
                              testing::end_line() + "\nmeasure.sh: restored governor=x\n";
  const MeasureScan scan = scan_measure_stderr(capture);
  EXPECT_EQ(scan.starts.size(), 1U);
  EXPECT_EQ(scan.ends.size(), 1U);
  EXPECT_EQ(scan.malformed.size(), 1U);
  EXPECT_EQ(scan.other_lines, 3U);
}

TEST(MeasureScan, HandlesEmptyAndUnterminatedInput) {
  EXPECT_TRUE(scan_measure_stderr("").starts.empty());
  EXPECT_EQ(scan_measure_stderr(testing::start_line()).starts.size(), 1U);
}

TEST(CpuList, Expands) {
  EXPECT_EQ(*expand_cpu_list("4"), (std::vector<int>{4}));
  EXPECT_EQ(*expand_cpu_list("0-2,6"), (std::vector<int>{0, 1, 2, 6}));
  for (const std::string_view bad : {"", ",", "1,", "-1", "3-1", "a", "1-", "9999", "0-9999"}) {
    EXPECT_FALSE(expand_cpu_list(bad).has_value()) << bad;
  }
}

}  // namespace
}  // namespace aadp::results
