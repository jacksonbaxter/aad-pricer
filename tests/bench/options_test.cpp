#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "harness.hpp"

namespace aadp::bench {
namespace {

std::expected<HarnessOptions, std::string> parse(std::vector<std::string> args) {
  args.insert(args.begin(), "bench");
  std::vector<char*> argv;
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  return parse_harness_options(static_cast<int>(argv.size()), argv.data());
}

TEST(HarnessOptions, TakesItsOwnFlagsAndPassesTheRestOn) {
  const auto options =
      parse({"--pin-cpu=4", "--reportable", "--source-root=.", "--batches=10", "--repetitions=2",
             "--warmup-ms=1", "--gb-warmup-s=0.5", "--batch-units=8", "--benchmark_filter=x"});
  ASSERT_TRUE(options.has_value()) << options.error();
  EXPECT_EQ(options->pin_cpu, 4);
  EXPECT_TRUE(options->reportable);
  EXPECT_EQ(options->batches, 10);
  EXPECT_EQ(options->repetitions, 2);
  EXPECT_EQ(options->warmup_ms, 1);
  EXPECT_DOUBLE_EQ(options->gb_warmup_s, 0.5);
  EXPECT_EQ(options->batch_units, 8U);
  EXPECT_EQ(options->passthrough, (std::vector<std::string>{"bench", "--benchmark_filter=x"}));
}

TEST(HarnessOptions, RefusesBadValues) {
  for (const std::string arg : {"--pin-cpu=", "--pin-cpu=-1", "--pin-cpu=x", "--batches=0",
                                "--batches=1x", "--repetitions=0", "--warmup-ms=-1",
                                "--gb-warmup-s=0", "--gb-warmup-s=x", "--batch-units=0"}) {
    EXPECT_FALSE(parse({arg}).has_value()) << arg;
  }
}

}  // namespace
}  // namespace aadp::bench
