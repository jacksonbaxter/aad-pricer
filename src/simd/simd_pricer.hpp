// The SIMD primal: pricer::Price with paths on Highway lanes, namespace aadp::simd.
//
//   PriceLanes(contract, market, config, lanes)   lanes in LaneWidths()
//       -> the Estimate Price() gives, bit for bit, or the reason it refused
//
// Static dispatch (cmake/AadpSimd.cmake): one Highway target per build, and LaneWidths() is every
// power of two from 1 to that target's double lanes, each its own instantiation of the same
// kernels (FixedTag<double, L>). The widest is the build's lane ceiling: 2 on NEON and SSE2, 4 on
// AVX2, 8 on AVX-512, 1 on HWY_SCALAR.
//
// The *Array functions run one math kernel over an array at the widest width, as non-inline
// symbols, so a benchmark can time them and a disassembly can show their inner loops.
#ifndef AADP_SIMD_SIMD_PRICER_HPP_
#define AADP_SIMD_SIMD_PRICER_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "pricer/contract.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::simd {

// The build's Highway target, as hwy::TargetName gives it, and its double lanes.
std::string TargetName();
std::size_t MaxLanes();
// 1, 2, 4, ... up to MaxLanes().
std::span<const std::size_t> LaneWidths();

struct LanePriceError {
  pricer::PriceError price;
  bool lanes = false;  // The lane count is not one of LaneWidths().
};

std::expected<pricer::Estimate, LanePriceError> PriceLanes(const pricer::Contract& contract,
                                                           const pricer::Market<double>& market,
                                                           const pricer::RunConfig& config,
                                                           std::size_t lanes);

// out[i] = kernel(in[i]) for i < min(in.size(), out.size()), at MaxLanes() lanes; a tail shorter
// than a vector goes through the one-lane instantiation.
void ExpArray(std::span<const double> in, std::span<double> out);
void LogArray(std::span<const double> in, std::span<double> out);
void InverseNormalArray(std::span<const double> in, std::span<double> out);
void NormalCdfArray(std::span<const double> in, std::span<double> out);

}  // namespace aadp::simd

#endif  // AADP_SIMD_SIMD_PRICER_HPP_
