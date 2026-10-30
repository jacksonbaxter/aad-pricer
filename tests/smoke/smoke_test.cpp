#include "smoke/smoke.hpp"

#include <benchmark/benchmark.h>
#include <gtest/gtest.h>
#include <hdr/hdr_histogram.h>

#include <cstdio>
#include <string_view>

#include "build_info.hpp"

namespace {

TEST(Smoke, HighwayMatchesTheConfiguredTarget) {
  std::printf("Highway target: %s\n", aadp::smoke::highway_target_name());
  const std::string_view target = aadp::build_info::simd_target;
  const double lanes = target == "scalar"   ? 1.0
                       : target == "avx2"   ? 4.0
                       : target == "avx512" ? 8.0
                                            : 2.0;
  EXPECT_EQ(aadp::smoke::sum_of_ones(), lanes);
}

TEST(Smoke, HdrHistogramRecords) {
  hdr_histogram* histogram = nullptr;
  ASSERT_EQ(hdr_init(1, 1'000'000, 3, &histogram), 0);
  EXPECT_TRUE(hdr_record_value(histogram, 42));
  EXPECT_EQ(hdr_value_at_percentile(histogram, 50.0), 42);
  hdr_close(histogram);
}

TEST(Smoke, BenchmarkLinks) {
  int calls = 0;
  benchmark::RegisterBenchmark("noop", [&calls](benchmark::State& state) {
    for (auto _ : state) {
      benchmark::DoNotOptimize(calls);
    }
    ++calls;
  });
  benchmark::ClearRegisteredBenchmarks();
  EXPECT_EQ(calls, 0);
}

TEST(Smoke, BuildInfoIsFilledIn) {
  EXPECT_FALSE(aadp::build_info::compiler_version.empty());
  EXPECT_NE(aadp::build_info::cxx_flags.find("-ffp-contract=off"), std::string_view::npos);
  EXPECT_EQ(aadp::build_info::cxx_standard, 23);
}

}  // namespace
