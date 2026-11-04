// pricer::RunBatches with paths on lanes. The same batches, the same blocks of
// pricer::kBlockPaths, the same work sharing, and the same sums in the same order, so with the
// kernels' per-path bits the same as the scalar's the batch means are the same bits too, for any
// lane count and any thread count.
//
// Within a block, paths go L at a time: lane i of a group starting at path p is path p + i. A
// group that runs past the block's end still computes every lane (the extra lanes are other
// paths' draws, which are valid), and only the lanes inside the block are added. Each statistic
// of each lane is added to the block's sum in path order, one at a time, exactly as the scalar
// runner adds them. A vector accumulator summed across lanes at the end would be faster, and would
// give other bits for any L > 1.
//
// make_lane_fn() returns a callable (const LaneUniforms<LaneReal<D>>&,
// std::array<LaneReal<D>, M>&) -> void, built once per worker, as in runner.hpp.
#ifndef AADP_SIMD_LANE_RUNNER_HPP_
#define AADP_SIMD_LANE_RUNNER_HPP_

#include <hwy/highway.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include "pricer/runner.hpp"
#include "simd/lane_kernels.hpp"
#include "simd/lane_real.hpp"
#include "simd/philox_lanes.hpp"

HWY_BEFORE_NAMESPACE();

namespace aadp::simd {

namespace hn = hwy::HWY_NAMESPACE;

// pricer::FillPath for paths first, first + 1, ..., one per lane.
template <class D>
void FillLanes(D d, std::uint64_t seed, std::uint64_t first, std::span<LaneReal<D>> normal,
               std::span<LaneReal<D>> crossing) {
  for (std::size_t step = 0; step < normal.size(); ++step) {
    const StepUniformLanes<D> u = DrawStepLanes(d, seed, first, static_cast<std::uint32_t>(step));
    normal[step] = LaneReal<D>(u.normal);
    if (!crossing.empty()) crossing[step] = LaneReal<D>(u.crossing);
  }
}

// One block's sums: paths first..last-1, L at a time, each lane's statistics added in path order.
template <std::size_t M, class D, class LaneFn>
std::array<double, M> SumBlock(D d, std::uint64_t seed, std::uint64_t first, std::uint64_t last,
                               LaneFn& lane_fn, std::span<LaneReal<D>> normal,
                               std::span<LaneReal<D>> crossing) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  const LaneUniforms<LaneReal<D>> uniforms{.normal = normal, .crossing = crossing};
  std::array<LaneReal<D>, M> stats{};
  std::array<std::array<double, kLanes>, M> lanes{};
  std::array<double, M> sum{};
  for (std::uint64_t group = first; group < last; group += kLanes) {
    FillLanes(d, seed, group, normal, crossing);
    lane_fn(uniforms, stats);
    for (std::size_t i = 0; i < M; ++i) hn::StoreU(stats[i].vec(), d, lanes[i].data());
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(kLanes, last - group));
    for (std::size_t lane = 0; lane < count; ++lane) {
      for (std::size_t i = 0; i < M; ++i) sum[i] += lanes[i][lane];
    }
  }
  return sum;
}

// Batch means of M statistics per path, in batch order. Precondition: ValidateRun passed.
template <std::size_t M, class D, class MakeLaneFn>
std::vector<std::array<double, M>> RunLaneBatches(D d, const pricer::RunConfig& config,
                                                  const pricer::PathShape& shape,
                                                  const MakeLaneFn& make_lane_fn) {
  using pricer::kBlockPaths;
  const std::uint64_t paths_per_batch = config.paths / config.batches;
  const std::uint64_t blocks_per_batch = (paths_per_batch + kBlockPaths - 1) / kBlockPaths;
  const std::uint64_t block_count = blocks_per_batch * config.batches;
  std::vector<std::array<double, M>> block_sums(static_cast<std::size_t>(block_count));

  std::atomic<std::uint64_t> next_block{0};
  const auto worker = [&] {
    // NOLINTNEXTLINE(misc-const-correctness): a lane function may carry state, such as a tape.
    auto lane_fn = make_lane_fn();
    std::vector<LaneReal<D>> normal(shape.steps);
    std::vector<LaneReal<D>> crossing(shape.crossing ? shape.steps : 0);
    for (;;) {
      const std::uint64_t block = next_block.fetch_add(1, std::memory_order_relaxed);
      if (block >= block_count) return;
      const std::uint64_t batch = block / blocks_per_batch;
      const std::uint64_t first =
          batch * paths_per_batch + (block % blocks_per_batch) * kBlockPaths;
      const std::uint64_t last = std::min(first + kBlockPaths, (batch + 1) * paths_per_batch);
      std::array<double, M> sum =
          SumBlock<M>(d, config.seed, first, last, lane_fn, std::span(normal), std::span(crossing));
      pricer::EndBlock(lane_fn, sum);
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

}  // namespace aadp::simd

HWY_AFTER_NAMESPACE();

#endif  // AADP_SIMD_LANE_RUNNER_HPP_
