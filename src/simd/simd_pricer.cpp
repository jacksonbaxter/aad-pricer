#include "simd/simd_pricer.hpp"

#include <hwy/highway.h>
#include <hwy/targets.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "simd/lane_kernels.hpp"
#include "simd/lane_math.hpp"
#include "simd/lane_real.hpp"
#include "simd/lane_runner.hpp"

HWY_BEFORE_NAMESPACE();

namespace aadp::simd {
namespace {

namespace hn = hwy::HWY_NAMESPACE;

constexpr std::size_t kMaxLanes = hn::MaxLanes(hn::ScalableTag<double>());
static_assert(kMaxLanes >= 1 && kMaxLanes <= 8 && (kMaxLanes & (kMaxLanes - 1)) == 0);

constexpr auto kWidths = [] {
  std::array<std::size_t, 4> widths{};
  std::size_t count = 0;
  for (std::size_t lanes = 1; lanes <= kMaxLanes; lanes *= 2) widths[count++] = lanes;
  return widths;
}();
constexpr std::size_t kWidthCount = [] {
  std::size_t count = 0;
  for (std::size_t lanes = 1; lanes <= kMaxLanes; lanes *= 2) ++count;
  return count;
}();

template <class D>
std::vector<double> BatchMeans(D d, const pricer::Contract& contract,
                               const pricer::Market<double>& market,
                               const pricer::RunConfig& config) {
  using Real = LaneReal<D>;
  const pricer::GbmModel<double> scalar(market, contract.steps);
  const LaneGbm<Real> model(scalar);
  const pricer::PathShape shape{.steps = contract.steps,
                                .crossing = pricer::NeedsCrossingUniforms(contract)};
  const auto table = RunLaneBatches<1>(d, config, shape, [&] {
    return [&](const LaneUniforms<Real>& u, std::array<Real, 1>& out) {
      out[0] = EvaluateLanes<Real>(contract, model, u).value;
    };
  });
  std::vector<double> means;
  means.reserve(table.size());
  for (const auto& row : table) means.push_back(row[0]);
  return means;
}

template <std::size_t L>
bool TryBatchMeans(std::size_t lanes, const pricer::Contract& contract,
                   const pricer::Market<double>& market, const pricer::RunConfig& config,
                   std::vector<double>& out) {
  if constexpr (L <= kMaxLanes) {
    if (lanes != L) return false;
    out = BatchMeans(hn::FixedTag<double, L>(), contract, market, config);
    return true;
  } else {
    return false;
  }
}

// Kernel over an array: whole vectors of L lanes, then the rest one lane at a time.
template <std::size_t L, class Kernel>
void ApplyAt(std::span<const double> in, std::span<double> out, const Kernel& kernel) {
  const std::size_t n = std::min(in.size(), out.size());
  const hn::FixedTag<double, L> d;
  std::size_t i = 0;
  for (; i + L <= n; i += L) hn::StoreU(kernel(d, hn::LoadU(d, in.data() + i)), d, out.data() + i);
  const hn::FixedTag<double, 1> d1;
  for (; i < n; ++i) hn::StoreU(kernel(d1, hn::LoadU(d1, in.data() + i)), d1, out.data() + i);
}

template <class Kernel>
bool Apply(std::span<const double> in, std::span<double> out, std::size_t lanes,
           const Kernel& kernel) {
  if (lanes == 1) {
    ApplyAt<1>(in, out, kernel);
  } else if (lanes == kMaxLanes) {
    ApplyAt<kMaxLanes>(in, out, kernel);
  } else {
    return false;
  }
  return true;
}

}  // namespace

std::string TargetName() { return hwy::TargetName(HWY_STATIC_TARGET); }

std::size_t MaxLanes() { return kMaxLanes; }

std::span<const std::size_t> LaneWidths() { return std::span(kWidths).first(kWidthCount); }

std::expected<pricer::Estimate, LanePriceError> PriceLanes(const pricer::Contract& contract,
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
  pricer::Estimate estimate;
  const bool ran = TryBatchMeans<1>(lanes, contract, market, config, estimate.batch_means) ||
                   TryBatchMeans<2>(lanes, contract, market, config, estimate.batch_means) ||
                   TryBatchMeans<4>(lanes, contract, market, config, estimate.batch_means) ||
                   TryBatchMeans<8>(lanes, contract, market, config, estimate.batch_means);
  if (!ran) return std::unexpected(LanePriceError{.price = {}, .lanes = true});
  const BatchStatistics stats = BatchMeansStatistics(estimate.batch_means);
  estimate.mean = stats.mean;
  estimate.standard_error = stats.standard_error;
  return estimate;
}

bool ExpArray(std::span<const double> in, std::span<double> out, std::size_t lanes) {
  return Apply(in, out, lanes, [](auto d, auto v) { return vmath::Exp(d, v); });
}
bool LogArray(std::span<const double> in, std::span<double> out, std::size_t lanes) {
  return Apply(in, out, lanes, [](auto d, auto v) { return vmath::Log(d, v); });
}
bool InverseNormalArray(std::span<const double> in, std::span<double> out, std::size_t lanes) {
  return Apply(in, out, lanes, [](auto d, auto v) { return vmath::InverseNormal(d, v); });
}
bool NormalCdfArray(std::span<const double> in, std::span<double> out, std::size_t lanes) {
  return Apply(in, out, lanes, [](auto d, auto v) { return vmath::NormalCdf(d, v); });
}

}  // namespace aadp::simd

HWY_AFTER_NAMESPACE();
