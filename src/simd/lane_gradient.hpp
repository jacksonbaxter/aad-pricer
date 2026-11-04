// Price and every first-order Greek with paths on lanes, through the vector tape, namespace
// aadp::simd.
//
//   PriceWithGradientLanes(contract, market, config, lanes)   lanes in LaneWidths()
//       -> ad::PriceWithGradient's GradientEstimate, bit for bit, and how many groups and paths
//          fell back to the scalar tape; or the reason it refused
//   MeasureLaneTape(contract, market, config, lanes)
//       -> what the vector tape holds per group on the first block's paths, for the benchmark
//
// How: src/simd/lane_path_gradient.hpp. Why the bits are the scalar gradient's: NOTES.md, "Vector
// tape: design and oracle".
#ifndef AADP_SIMD_LANE_GRADIENT_HPP_
#define AADP_SIMD_LANE_GRADIENT_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>

#include "ad/gradient.hpp"
#include "pricer/contract.hpp"
#include "pricer/runner.hpp"
#include "simd/simd_pricer.hpp"

namespace aadp::simd {

struct LaneGradientEstimate {
  ad::GradientEstimate estimate;
  // Groups whose lanes diverged, and the paths in them, priced on the scalar tape instead. Never
  // silent: zero for every product the pricer has.
  std::uint64_t fallback_groups = 0;
  std::uint64_t fallback_paths = 0;
};

std::expected<LaneGradientEstimate, LanePriceError> PriceWithGradientLanes(
    const pricer::Contract& contract, const pricer::Market<double>& market,
    const pricer::RunConfig& config, std::size_t lanes);

// The vector tape after recording each group of the first min(paths, kBlockPaths) paths of
// config.seed, before its sweep: the largest group's statements and operands. Bytes per path are
// its statement and operand bytes over its lanes, as tests/simd/lane_tape_memory_test.cpp counts.
struct LaneTapeMemory {
  std::size_t lanes = 0;
  std::size_t statements = 0;  // Of the largest group
  std::size_t operands = 0;
  std::size_t statement_bytes = 0;  // Per statement
  std::size_t operand_bytes = 0;    // Per operand: 12 at one lane, 5 + 8 lanes above
  std::size_t allocated_bytes = 0;  // Every buffer the lane tape holds after the last group
  std::size_t groups = 0;
};

std::expected<LaneTapeMemory, LanePriceError> MeasureLaneTape(const pricer::Contract& contract,
                                                              const pricer::Market<double>& market,
                                                              const pricer::RunConfig& config,
                                                              std::size_t lanes);

}  // namespace aadp::simd

#endif  // AADP_SIMD_LANE_GRADIENT_HPP_
