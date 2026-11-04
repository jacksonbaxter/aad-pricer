// Exercises the harness end to end with a small fixed floating-point loop. It measures the
// harness, not this project: nothing about pricing happens here, and its numbers are not
// results of any kind.
#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

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

// The workload for --no-work: a batch is then a call and two timer reads, and the timer's
// resolution is at least what one read costs, so the batch is below the gate (ten resolutions,
// and at least 100 ns) on any host and under any instrumentation. One unit of real work is not:
// under Linux TSan on GitHub's runners and on debian it takes about 200 ns, right at the gate.
void no_work(std::uint64_t /*units*/) {}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  std::vector<char*> args(argv, argv + argc);
  const auto removed = std::erase_if(
      args, [](const char* arg) { return arg != nullptr && std::string_view(arg) == "--no-work"; });
  const auto count = static_cast<int>(args.size());
  args.push_back(nullptr);  // argv[argc], which the libraries may read
  return aadp::bench::harness_main(
      count, args.data(),
      {{.name = "harness_selftest", .run = removed != 0 ? no_work : selftest_workload}});
} catch (const std::exception& error) {
  std::fprintf(stderr, "harness_selftest: %s\n", error.what());
  return 1;
}
