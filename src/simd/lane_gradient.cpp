#include "simd/lane_gradient.hpp"

#include <hwy/highway.h>

#include <cstddef>
#include <expected>

#include "pricer/contract.hpp"
#include "pricer/runner.hpp"
#include "simd/lane_path_gradient.hpp"
#include "simd/simd_pricer.hpp"

HWY_BEFORE_NAMESPACE();

namespace aadp::simd {
namespace {

namespace hn = hwy::HWY_NAMESPACE;

constexpr std::size_t kMaxLanes = hn::MaxLanes(hn::ScalableTag<double>());

template <std::size_t L>
bool TryLanes(std::size_t lanes, const pricer::Contract& contract,
              const pricer::Market<double>& market, const pricer::RunConfig& config,
              LaneGradientEstimate& out) {
  if constexpr (L <= kMaxLanes) {
    if (lanes != L) return false;
    out = RunLaneGradient(hn::FixedTag<double, L>(), contract, market, config);
    return true;
  } else {
    return false;
  }
}

}  // namespace

std::expected<LaneGradientEstimate, LanePriceError> PriceWithGradientLanes(
    const pricer::Contract& contract, const pricer::Market<double>& market,
    const pricer::RunConfig& config, std::size_t lanes) {
  const pricer::ContractError contract_error = pricer::Validate(contract, market);
  if (contract_error != pricer::ContractError::kNone) {
    return std::unexpected(LanePriceError{.price = {.contract = contract_error}});
  }
  const pricer::RunError run_error = pricer::ValidateRun(config);
  if (run_error != pricer::RunError::kNone) {
    return std::unexpected(LanePriceError{.price = {.run = run_error}});
  }
  LaneGradientEstimate estimate;
  const bool ran = TryLanes<1>(lanes, contract, market, config, estimate) ||
                   TryLanes<2>(lanes, contract, market, config, estimate) ||
                   TryLanes<4>(lanes, contract, market, config, estimate) ||
                   TryLanes<8>(lanes, contract, market, config, estimate);
  if (!ran) return std::unexpected(LanePriceError{.price = {}, .lanes = true});
  return estimate;
}

}  // namespace aadp::simd

HWY_AFTER_NAMESPACE();
