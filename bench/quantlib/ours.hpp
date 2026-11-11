// Our side of the QuantLib comparison: the production pricer on each comparison's contract, on one
// thread, as "ours, scalar" (pricer::Price, or the tape for R9) and on the build's widest lanes
// (simd::PriceLanes, or the vector tape), which give the same bits. R3's control variate has no
// lane form.
#ifndef AADP_BENCH_QUANTLIB_OURS_HPP_
#define AADP_BENCH_QUANTLIB_OURS_HPP_

#include <cstdint>
#include <expected>
#include <string>

#include "pricer/contract.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "quantlib/compare.hpp"

namespace aadp::qlcompare {

enum class OurVariant : std::uint8_t { kScalar, kLanes };

bool HasLaneForm(const Comparison& comparison);

// The comparison's estimate (the delta for R9) on one thread. Precondition: comparison.engine is
// one of ours.
std::expected<pricer::Estimate, std::string> RunOurs(const Comparison& comparison,
                                                     OurVariant variant, std::uint64_t seed,
                                                     std::uint64_t paths, std::uint32_t batches);

// R3: per path, the arithmetic payoff minus the geometric payoff on the same path plus the
// geometric closed form over the same points, all discounted: QuantLib's control with coefficient
// 1 (EVIDENCE E4). Precondition: an arithmetic Asian call that Validate accepts.
std::expected<pricer::Estimate, std::string> PriceControlVariate(
    const pricer::Contract& arithmetic, const pricer::Market<double>& market,
    const pricer::RunConfig& config);

}  // namespace aadp::qlcompare

#endif  // AADP_BENCH_QUANTLIB_OURS_HPP_
