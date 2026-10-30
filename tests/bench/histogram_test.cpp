#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "harness.hpp"
#include "latency.hpp"

namespace aadp::bench {
namespace {

TEST(BatchGate, FloorsAt100Nanoseconds) {
  EXPECT_DOUBLE_EQ(min_batch_span_ns(0.1), 100.0);
  EXPECT_DOUBLE_EQ(target_batch_span_ns(0.1), 100.0);
}

// Linux: about 10 ns granularity and about 20 ns between reads. Mac: 41.67 ns ticks.
TEST(BatchGate, ScalesWithMeasuredResolution) {
  EXPECT_DOUBLE_EQ(min_batch_span_ns(20.0), 200.0);
  EXPECT_DOUBLE_EQ(min_batch_span_ns(41.67), 416.7);
  EXPECT_GT(min_batch_span_ns(41.67), 3 * 41.67) << "a few 42 ns ticks is not enough";
  EXPECT_DOUBLE_EQ(target_batch_span_ns(41.67), 4167.0);
}

TEST(Histogram, RefusesWhenAnyBatchIsBelowTheGate) {
  const std::vector<std::int64_t> batches = {5000, 5000, 400, 5000};
  const auto histogram = LatencyHistogram::from_batches(batches, 416.7);
  ASSERT_FALSE(histogram.has_value());
  EXPECT_NE(histogram.error().find("refusing to produce a histogram"), std::string::npos);
}

TEST(Histogram, RefusesAtTheFloorOnAFineTimer) {
  const std::vector<std::int64_t> batches = {99, 99, 99};
  EXPECT_FALSE(LatencyHistogram::from_batches(batches, min_batch_span_ns(1.0)).has_value());
}

TEST(Histogram, RefusesNoBatchesAndNoGate) {
  EXPECT_FALSE(LatencyHistogram::from_batches({}, 100.0).has_value());
  const std::vector<std::int64_t> batches = {500};
  EXPECT_FALSE(LatencyHistogram::from_batches(batches, 0.0).has_value());
}

TEST(Histogram, RefusesAValueItCannotHold) {
  const std::vector<std::int64_t> batches = {7'200'000'000'000};
  EXPECT_FALSE(LatencyHistogram::from_batches(batches, 100.0).has_value());
}

TEST(Histogram, PublishesPercentilesOfBatchesAtOrAboveTheGate) {
  std::vector<std::int64_t> batches;
  for (std::int64_t i = 1; i <= 1000; ++i) {
    batches.push_back(1000 + i);
  }
  const auto histogram = LatencyHistogram::from_batches(batches, 417.0);
  ASSERT_TRUE(histogram.has_value()) << histogram.error();
  EXPECT_EQ(histogram->count(), 1000);
  const auto values = histogram->values();
  std::vector<std::string> labels;
  for (const PublishedValue& value : values) {
    labels.push_back(value.label);
    ASSERT_TRUE(value.ns.has_value()) << value.label;
    EXPECT_GE(*value.ns, 1000) << value.label;
    EXPECT_LE(*value.ns, 2010) << value.label;
  }
  EXPECT_EQ(labels,
            (std::vector<std::string>{"min", "p50", "p90", "p99", "p99.9", "p99.99", "max"}));
}

TEST(Publishable, WithholdsAValueTheTimerCannotResolve) {
  EXPECT_FALSE(publishable(416, 416.7).has_value());
  EXPECT_EQ(publishable(417, 416.7), 417);
}

TEST(Render, WritesEveryFieldTheTableReads) {
  const std::vector<std::int64_t> batches = {1000, 1000, 1000};
  const auto histogram = LatencyHistogram::from_batches(batches, 500.0);
  ASSERT_TRUE(histogram.has_value());
  const std::string body = render_histogram(4, *histogram, 41.67);
  EXPECT_NE(body.find("batch_units 4\n"), std::string::npos);
  EXPECT_NE(body.find("batches 3\n"), std::string::npos);
  EXPECT_NE(body.find("gate_ns 500.0\n"), std::string::npos);
  EXPECT_NE(body.find("timer_resolution_ns 41.670\n"), std::string::npos);
  EXPECT_NE(body.find("p50 1000\n"), std::string::npos);
  EXPECT_NE(body.find("p99 1000\n"), std::string::npos);
  EXPECT_NE(body.find("max 1000\n"), std::string::npos);
}

TEST(SizeBatch, GrowsUntilTheTargetIsReached) {
  const auto timed = [](std::uint64_t units) { return static_cast<double>(units) * 30.0; };
  const auto sizing = size_batch(timed, 4167.0);
  ASSERT_TRUE(sizing.has_value()) << sizing.error();
  EXPECT_EQ(sizing->units, 256U);
  EXPECT_DOUBLE_EQ(sizing->median_span_ns, 7680.0);
}

TEST(SizeBatch, UsesTheMedianSoOneSlowTrialDoesNotDecide) {
  int calls = 0;
  const auto timed = [&calls](std::uint64_t units) {
    return ++calls == 1 ? 1e9 : static_cast<double>(units);
  };
  const auto sizing = size_batch(timed, 64.0);
  ASSERT_TRUE(sizing.has_value());
  EXPECT_EQ(sizing->units, 64U);
}

TEST(SizeBatch, RefusesAWorkloadThatNeverReachesTheTarget) {
  const auto timed = [](std::uint64_t) { return 0.0; };
  EXPECT_FALSE(size_batch(timed, 100.0).has_value());
}

}  // namespace
}  // namespace aadp::bench
