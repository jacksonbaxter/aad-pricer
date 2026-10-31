// Exercises the harness end to end with a small fixed floating-point loop. It measures the
// harness, not this project: nothing about pricing happens here, and its numbers are not
// results of any kind.
#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>

#include "harness.hpp"
#include "platform/cacheline.hpp"

namespace {

constexpr std::size_t kValues = 16;

std::array<double, kValues> make_values() {
  std::array<double, kValues> values{};
  for (std::size_t i = 0; i < kValues; ++i) {
    values[i] = 1.0 + static_cast<double>(i) / static_cast<double>(kValues);
  }
  return values;
}

void selftest_workload(std::uint64_t units) {
  alignas(aadp::platform::kCacheLineBytes) static const std::array<double, kValues> values =
      make_values();
  double sum = 0.0;
  for (std::uint64_t unit = 0; unit < units; ++unit) {
    for (const double value : values) {
      sum = sum * 0.5 + value * value;
    }
    benchmark::DoNotOptimize(sum);
  }
}

}  // namespace

int main(int argc, char** argv) try {
  return aadp::bench::harness_main(argc, argv,
                                   {{.name = "harness_selftest", .run = selftest_workload}});
} catch (const std::exception& error) {
  std::fprintf(stderr, "harness_selftest: %s\n", error.what());
  return 1;
}
