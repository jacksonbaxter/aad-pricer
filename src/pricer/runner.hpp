// Runs paths on worker threads with a result that does not depend on the thread count.
//
// Paths split into B batches of N / B, and each batch into fixed blocks of kBlockPaths. Workers
// take whole blocks; each block's statistics are summed in path order into that block's own
// accumulator, and the blocks are merged in block order once every worker is done. No sum ever
// depends on which worker ran what, so 1, 2, 3 or 8 threads give the same bits as one.
//
// Per-worker state. make_path_fn is called once on each worker, and the callable it returns is
// used only there. That is where a later stage keeps a worker's tape.
#ifndef AADP_PRICER_RUNNER_HPP_
#define AADP_PRICER_RUNNER_HPP_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "pricer/draws.hpp"

namespace aadp::pricer {

inline constexpr std::uint64_t kBlockPaths = 1024;
inline constexpr std::uint32_t kMaxThreads = 256;

struct RunConfig {
  std::uint64_t seed = 0;
  std::uint64_t paths = 0;  // A positive multiple of batches.
  std::uint32_t batches = 0;
  std::uint32_t threads = 1;
};

enum class RunError : std::uint8_t { kNone, kBatches, kPaths, kThreads };

constexpr RunError ValidateRun(const RunConfig& config) {
  if (config.batches < 2) return RunError::kBatches;
  if (config.paths == 0 || config.paths % config.batches != 0) return RunError::kPaths;
  if (config.threads < 1 || config.threads > kMaxThreads) return RunError::kThreads;
  return RunError::kNone;
}

struct PathShape {
  std::uint32_t steps = 1;
  bool crossing = false;  // Whether the kernel reads crossing uniforms.
};

// Batch means of M statistics per path, in batch order. Precondition: ValidateRun passed.
// make_path_fn() returns a callable (const PathUniforms&, std::array<double, M>&) -> void.
template <std::size_t M, class MakePathFn>
std::vector<std::array<double, M>> RunBatches(const RunConfig& config, const PathShape& shape,
                                              const MakePathFn& make_path_fn) {
  const std::uint64_t paths_per_batch = config.paths / config.batches;
  const std::uint64_t blocks_per_batch = (paths_per_batch + kBlockPaths - 1) / kBlockPaths;
  const std::uint64_t block_count = blocks_per_batch * config.batches;
  std::vector<std::array<double, M>> block_sums(static_cast<std::size_t>(block_count));

  std::atomic<std::uint64_t> next_block{0};
  const auto worker = [&] {
    auto path_fn = make_path_fn();
    std::vector<double> normal(shape.steps);
    std::vector<double> crossing(shape.crossing ? shape.steps : 0);
    const PathUniforms uniforms{.normal = normal, .crossing = crossing};
    std::array<double, M> stats{};
    for (;;) {
      const std::uint64_t block = next_block.fetch_add(1, std::memory_order_relaxed);
      if (block >= block_count) return;
      const std::uint64_t batch = block / blocks_per_batch;
      const std::uint64_t first =
          batch * paths_per_batch + (block % blocks_per_batch) * kBlockPaths;
      const std::uint64_t last = std::min(first + kBlockPaths, (batch + 1) * paths_per_batch);
      std::array<double, M> sum{};
      for (std::uint64_t path = first; path < last; ++path) {
        FillPath(config.seed, path, normal, crossing);
        path_fn(uniforms, stats);
        for (std::size_t i = 0; i < M; ++i) sum[i] += stats[i];
      }
      block_sums[static_cast<std::size_t>(block)] = sum;
    }
  };

  {
    std::vector<std::jthread> pool;
    pool.reserve(config.threads - 1);
    for (std::uint32_t t = 1; t < config.threads; ++t) pool.emplace_back(worker);
    worker();
  }

  std::vector<std::array<double, M>> batch_means(config.batches);
  const auto divisor = static_cast<double>(paths_per_batch);
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

}  // namespace aadp::pricer

#endif  // AADP_PRICER_RUNNER_HPP_
