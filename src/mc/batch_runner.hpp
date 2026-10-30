// Paths split into B batches of N / B, and each batch into fixed blocks. A worker takes any
// block, but blocks are summed in block order afterwards, so no number depends on which thread
// ran what. That makes thread count invisible in the result, bit for bit.
#ifndef AADP_MC_BATCH_RUNNER_HPP_
#define AADP_MC_BATCH_RUNNER_HPP_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include "rng/philox.hpp"

namespace aadp {

inline constexpr std::uint64_t kPathsPerBlock = 1024;

struct RunConfig {
  std::uint64_t seed = 0;
  std::uint64_t paths = 0;  // A multiple of batches.
  std::uint32_t batches = 0;
  std::uint32_t steps = 0;  // Uniforms per path.
  std::uint32_t threads = 1;
};

// Means per batch, in batch order. Stat count M is fixed at compile time. PathFn fills one
// path's statistics from its uniforms and must be safe to call from several threads.
template <std::size_t M, class PathFn>
std::vector<std::array<double, M>> RunBatches(const RunConfig& config, const PathFn& path_fn) {
  const std::uint64_t paths_per_batch = config.paths / config.batches;
  const std::uint64_t blocks_per_batch = (paths_per_batch + kPathsPerBlock - 1) / kPathsPerBlock;
  const std::uint64_t block_count = blocks_per_batch * config.batches;
  std::vector<std::array<double, M>> block_sums(static_cast<std::size_t>(block_count));

  std::atomic<std::uint64_t> next_block{0};
  const auto worker = [&] {
    std::vector<double> uniforms(config.steps);
    for (;;) {
      const std::uint64_t block = next_block.fetch_add(1, std::memory_order_relaxed);
      if (block >= block_count) return;
      const std::uint64_t batch = block / blocks_per_batch;
      const std::uint64_t first =
          batch * paths_per_batch + (block % blocks_per_batch) * kPathsPerBlock;
      const std::uint64_t last = std::min(first + kPathsPerBlock, (batch + 1) * paths_per_batch);
      std::array<double, M> sum{};
      std::array<double, M> stats{};
      for (std::uint64_t path = first; path < last; ++path) {
        for (std::uint32_t step = 0; step < config.steps; ++step) {
          uniforms[step] = PathUniform(config.seed, path, step);
        }
        path_fn(std::span<const double>(uniforms), stats);
        for (std::size_t i = 0; i < M; ++i) sum[i] += stats[i];
      }
      block_sums[static_cast<std::size_t>(block)] = sum;
    }
  };

  {
    std::vector<std::jthread> pool;
    for (std::uint32_t t = 1; t < config.threads; ++t) pool.emplace_back(worker);
    worker();
  }

  std::vector<std::array<double, M>> batch_means(config.batches);
  const double divisor = static_cast<double>(paths_per_batch);
  for (std::size_t batch = 0; batch < batch_means.size(); ++batch) {
    std::array<double, M> sum{};
    for (std::uint64_t i = 0; i < blocks_per_batch; ++i) {
      const auto& block = block_sums[static_cast<std::size_t>(batch * blocks_per_batch + i)];
      for (std::size_t j = 0; j < M; ++j) sum[j] += block[j];
    }
    for (std::size_t j = 0; j < M; ++j) batch_means[batch][j] = sum[j] / divisor;
  }
  return batch_means;
}

}  // namespace aadp

#endif  // AADP_MC_BATCH_RUNNER_HPP_
