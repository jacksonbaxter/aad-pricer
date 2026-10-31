#include <benchmark/benchmark.h>

#include "smoke/smoke.hpp"

namespace {

void BM_SumOfOnes(benchmark::State& state) {
  for ([[maybe_unused]] const auto _ : state) {
    benchmark::DoNotOptimize(aadp::smoke::sum_of_ones());
  }
}
BENCHMARK(BM_SumOfOnes);

}  // namespace
