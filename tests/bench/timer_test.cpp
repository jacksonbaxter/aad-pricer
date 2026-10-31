#include "platform/timer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

namespace aadp::platform {
namespace {

TEST(ReadStats, FindsGranularityFromMultiples) {
  // Reads that only land on multiples of 20, as a TSC with a coarse update does.
  const std::array<std::uint64_t, 6> reads = {1000, 1040, 1060, 1060, 1140, 1180};
  const ReadStats stats = analyse_reads(reads);
  EXPECT_EQ(stats.reads, 6U);
  EXPECT_EQ(stats.repeats, 1U);
  EXPECT_EQ(stats.backward, 0U);
  EXPECT_EQ(stats.granularity, 20U);
  EXPECT_EQ(stats.min_nonzero_delta, 20U);
}

TEST(ReadStats, SeparatesGranularityFromReadCost) {
  // The counter steps by 20 but two reads are never closer than 40.
  const std::array<std::uint64_t, 4> reads = {0, 60, 100, 160};
  const ReadStats stats = analyse_reads(reads);
  EXPECT_EQ(stats.granularity, 20U);
  EXPECT_EQ(stats.min_nonzero_delta, 40U);
}

TEST(ReadStats, CountsBackwardSteps) {
  const std::array<std::uint64_t, 3> reads = {100, 90, 120};
  EXPECT_EQ(analyse_reads(reads).backward, 1U);
}

TEST(ReadStats, AFrozenTimerHasNoResolution) {
  const std::array<std::uint64_t, 3> reads = {7, 7, 7};
  const ReadStats stats = analyse_reads(reads);
  EXPECT_EQ(stats.granularity, 0U);
  EXPECT_EQ(stats.min_nonzero_delta, 0U);
}

TEST(ReadStats, HandlesTooFewReads) {
  EXPECT_EQ(analyse_reads(std::span<const std::uint64_t>{}).granularity, 0U);
  const std::array<std::uint64_t, 1> one = {5};
  EXPECT_EQ(analyse_reads(one).reads, 1U);
}

TEST(Timer, ReportsAnObservedResolution) {
  const TimerReport report = calibrate_timer();
  EXPECT_EQ(timer_problem(report), "");
  EXPECT_GT(report.ticks_per_second, 0.0);
  EXPECT_GT(report.tick_reads.granularity, 0U);
  EXPECT_GT(report.tick_resolution_ns, 0.0);
  EXPECT_GE(report.tick_resolution_ns, report.tick_granularity_ns);
  EXPECT_GE(report.tick_resolution_ns, report.tick_min_delta_ns);
  EXPECT_GT(report.tick_read_cost_ns, 0.0);
  EXPECT_GT(report.clock_resolution_ns, 0.0);
  EXPECT_GT(report.clock_getres_ns, 0.0);
  EXPECT_STRNE(report.tick_source, "");
  EXPECT_STREQ(report.clock_source, "CLOCK_MONOTONIC_RAW");
}

TEST(Timer, ResolutionFollowsFromTheDeltasAndTheCalibratedRate) {
  const TimerReport report = calibrate_timer();
  const double expected =
      static_cast<double>(report.tick_reads.min_nonzero_delta) * 1e9 / report.ticks_per_second;
  EXPECT_DOUBLE_EQ(report.tick_min_delta_ns, expected);
}

// The limit is the one timer_problem enforces, because a wider spread would make every
// nanosecond figure in a results file unreliable.
TEST(Timer, TwoCalibrationsAgreeToOnePercent) {
  const double first = calibrate_timer().ticks_per_second;
  const double second = calibrate_timer().ticks_per_second;
  EXPECT_NEAR(first / second, 1.0, 0.01);
}

TEST(Timer, TicksAdvanceAgainstTheClock) {
  const TimerReport report = calibrate_timer();
  const std::uint64_t ticks_start = read_ticks();
  const std::uint64_t ns_start = clock_ns();
  while (clock_ns() - ns_start < 20'000'000) {
  }
  const double elapsed_ns =
      static_cast<double>(read_ticks() - ticks_start) * 1e9 / report.ticks_per_second;
  EXPECT_GT(elapsed_ns, 19'000'000.0);
}

TEST(TimerProblem, FlagsUnusableReports) {
  TimerReport report;
  EXPECT_NE(timer_problem(report), "");
  report.ticks_per_second = 1e6;
  EXPECT_NE(timer_problem(report), "");
  report.tick_reads.granularity = 1;
  EXPECT_EQ(timer_problem(report), "");
  report.tick_reads.backward = 1;
  EXPECT_NE(timer_problem(report), "");
  report.tick_reads.backward = 0;
  report.calibration_spread = 0.02;
  EXPECT_NE(timer_problem(report), "");
}

}  // namespace
}  // namespace aadp::platform
