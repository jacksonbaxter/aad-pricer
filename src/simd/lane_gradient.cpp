#include "simd/lane_gradient.hpp"

#include <hwy/highway.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "ad/tape.hpp"
#include "pricer/contract.hpp"
#include "pricer/kernels.hpp"
#include "pricer/runner.hpp"
#include "simd/lane_kernels.hpp"
#include "simd/lane_path_gradient.hpp"
#include "simd/lane_real.hpp"
#include "simd/lane_runner.hpp"
#include "simd/lane_tape.hpp"
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

template <class D>
LaneTapeMemory Measure(D d, const pricer::Contract& contract, const pricer::Market<double>& market,
                       const pricer::RunConfig& config) {
  using Tape = LaneTape<D>;
  constexpr std::size_t kLanes = Tape::kLanes;
  LaneTapeMemory out;
  out.lanes = kLanes;
  out.statement_bytes = Tape::kStatementBytes;
  out.operand_bytes = Tape::kOperandBytes;
  LanePathGradient<D> lanes(contract, market);
  const Tape& tape = lanes.tape();
  std::vector<LaneReal<D>> normal(contract.steps);
  std::vector<LaneReal<D>> crossing(pricer::NeedsCrossingUniforms(contract) ? contract.steps : 0);
  const LaneUniforms<LaneReal<D>> u{.normal = normal, .crossing = crossing};
  const std::uint64_t paths = std::min<std::uint64_t>(config.paths, pricer::kBlockPaths);
  for (std::uint64_t first = 0; first < paths; first += kLanes) {
    FillLanes(d, config.seed, first, std::span(normal), std::span(crossing));
    static_cast<void>(lanes.Record(u));
    const ad::MemoryReport m = tape.Memory();
    if (m.statements > out.statements) {
      out.statements = m.statements;
      out.operands = m.operands;
    }
    lanes.ClearGroup();
    out.groups += 1;
  }
  out.allocated_bytes = tape.Memory().allocated_bytes;
  return out;
}

template <std::size_t L>
bool TryMeasure(std::size_t lanes, const pricer::Contract& contract,
                const pricer::Market<double>& market, const pricer::RunConfig& config,
                LaneTapeMemory& out) {
  if constexpr (L <= kMaxLanes) {
    if (lanes != L) return false;
    out = Measure(hn::FixedTag<double, L>(), contract, market, config);
    return true;
  } else {
    return false;
  }
}

}  // namespace

std::expected<LaneTapeMemory, LanePriceError> MeasureLaneTape(const pricer::Contract& contract,
                                                              const pricer::Market<double>& market,
                                                              const pricer::RunConfig& config,
                                                              std::size_t lanes) {
  const pricer::ContractError contract_error = pricer::Validate(contract, market);
  if (contract_error != pricer::ContractError::kNone) {
    return std::unexpected(LanePriceError{.price = {.contract = contract_error}});
  }
  const pricer::RunError run_error = pricer::ValidateRun(config);
  if (run_error != pricer::RunError::kNone) {
    return std::unexpected(LanePriceError{.price = {.run = run_error}});
  }
  LaneTapeMemory memory;
  const bool ran = TryMeasure<1>(lanes, contract, market, config, memory) ||
                   TryMeasure<2>(lanes, contract, market, config, memory) ||
                   TryMeasure<4>(lanes, contract, market, config, memory) ||
                   TryMeasure<8>(lanes, contract, market, config, memory);
  if (!ran) return std::unexpected(LanePriceError{.price = {}, .lanes = true});
  return memory;
}

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
