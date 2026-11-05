#include "pricer/local_vol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "pricer/substep_kernels.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::pricer {

namespace {

inline constexpr double kMinRuleVariance = 1e-8;
inline constexpr double kMaxRuleVariance = kMaxVolatility * kMaxVolatility;

double Magnitude(double x) { return x < 0.0 ? -x : x; }

// The largest |d ln F / dt| over the curve's segments.
double MaxCarry(std::span<const svi::SliceTerms> terms) {
  double previous_time = 0.0;
  double previous_log = math::log(terms.front().discount * terms.front().forward);
  double carry = 0.0;
  for (const svi::SliceTerms& t : terms) {
    const double log_forward = math::log(t.forward);
    carry = std::max(carry, Magnitude((log_forward - previous_log) / (t.expiry - previous_time)));
    previous_time = t.expiry;
    previous_log = log_forward;
  }
  return carry;
}

// pricer.cpp's argument with the cap's volatility in every step: an unconditioned step moves
// log S by at most |drift| + 8.3 sd, a conditioned one by at most 2 |drift| + 8.6 sd, with
// sd <= sqrt(cap dt) and |drift| <= (max carry + cap / 2) dt.
bool WithinRange(const LocalVolContract& lv, std::span<const svi::SliceTerms> terms, double spot,
                 const svi::LocalVolRule& rule, double log_discount) {
  const Contract& c = lv.contract;
  double log_scale = std::max(Magnitude(math::log(spot)), Magnitude(math::log(c.strike)));
  if (c.product == Product::kBarrier) {
    log_scale = std::max(log_scale, Magnitude(math::log(c.barrier.level)));
  }
  const double steps = static_cast<double>(c.steps) * static_cast<double>(lv.substeps);
  const double drift = (MaxCarry(terms) + (0.5 * rule.cap)) * lv.maturity;
  const double diffusion = kMaxDrawMagnitude * math::sqrt(rule.cap * lv.maturity * steps);
  return log_scale + (2.0 * drift) + diffusion <= kMaxLogExcursion &&
         Magnitude(log_discount) <= kMaxLogExcursion;
}

}  // namespace

const char* LocalVolErrorName(LocalVolError error) noexcept {
  switch (error) {
    case LocalVolError::kNone:
      return "none";
    case LocalVolError::kSurface:
      return "the surface failed validation";
    case LocalVolError::kRule:
      return "floor and cap must satisfy 1e-8 <= floor < cap <= 100";
    case LocalVolError::kProduct:
      return "only European, arithmetic Asian and knock-out barriers, without include_spot";
    case LocalVolError::kContract:
      return "the contract failed validation";
    case LocalVolError::kMaturity:
      return "maturity past the last slice: the surface is not extrapolated";
    case LocalVolError::kSubsteps:
      return "sub-steps below 1, or dates times sub-steps above the step limit";
    case LocalVolError::kRange:
      return "parameters could carry a path outside the finite range";
    case LocalVolError::kRun:
      return "the run configuration failed validation";
  }
  return "unknown error";
}

LocalVolFailure ValidateLocalVol(const LocalVolContract& lv, std::span<const svi::SliceTerms> terms,
                                 std::span<const svi::RawSvi<double>> params,
                                 const svi::LocalVolRule& rule) {
  if (!(rule.floor >= kMinRuleVariance && rule.floor < rule.cap && rule.cap <= kMaxRuleVariance)) {
    return {.code = LocalVolError::kRule};
  }
  const svi::SurfaceError surface = svi::ValidateSurface(terms, params);
  if (surface != svi::SurfaceError::kNone) {
    return {.code = LocalVolError::kSurface, .surface = surface};
  }
  const Contract& c = lv.contract;
  if (static_cast<std::uint8_t>(c.product) >= kProductCount || !SubstepSupports(c) ||
      c.include_spot) {
    return {.code = LocalVolError::kProduct};
  }
  const svi::ForwardCurve curve(terms);
  const Market<double> market{.spot = curve.Spot(),
                              .sigma = math::sqrt(rule.cap),
                              .r = 0.0,
                              .q = 0.0,
                              .maturity = lv.maturity};
  const ContractError contract = Validate(c, market);
  if (contract != ContractError::kNone) {
    return {.code = LocalVolError::kContract, .contract = contract};
  }
  if (!(lv.maturity <= curve.LastExpiry())) return {.code = LocalVolError::kMaturity};
  if (lv.substeps < 1 ||
      static_cast<std::uint64_t>(c.steps) * lv.substeps > static_cast<std::uint64_t>(kMaxSteps)) {
    return {.code = LocalVolError::kSubsteps};
  }
  if (!WithinRange(lv, terms, curve.Spot(), rule, curve.LogDiscount(lv.maturity))) {
    return {.code = LocalVolError::kRange};
  }
  return {};
}

std::expected<LocalVolEstimate, LocalVolFailure> PriceLocalVol(
    const LocalVolContract& lv, std::span<const svi::SliceTerms> terms,
    std::span<const svi::RawSvi<double>> params, const RunConfig& config,
    const svi::LocalVolRule& rule) {
  const LocalVolFailure failure = ValidateLocalVol(lv, terms, params, rule);
  if (failure.code != LocalVolError::kNone) return std::unexpected(failure);
  const RunError run = ValidateRun(config);
  if (run != RunError::kNone) {
    return std::unexpected(LocalVolFailure{.code = LocalVolError::kRun, .run = run});
  }

  const Contract& c = lv.contract;
  const std::uint32_t n = lv.substeps;
  const std::uint32_t steps = c.steps * n;
  const LocalVolModel<double> model(svi::SviSurface<double>(terms, params), lv.maturity, steps,
                                    rule);
  // One tally per worker: RunBatches calls make_path_fn once on each of config.threads workers.
  std::vector<FloorTally> tallies(config.threads);
  std::atomic<std::size_t> next{0};
  const PathShape shape{.steps = steps, .crossing = NeedsCrossingUniforms(c)};
  const auto table = RunBatches<1>(config, shape, [&] {
    FloorTally* tally = &tallies[next.fetch_add(1, std::memory_order_relaxed)];
    LocalVolModel<double> own = model;
    own.SetTally(tally);
    return [&c, n, tally, own](const PathUniforms& u, std::array<double, 1>& out) {
      const std::uint64_t floored = tally->Floored();
      const std::uint64_t capped = tally->Capped();
      out[0] = EvaluateSubstepped<double>(c, n, own, u).value;
      tally->paths += 1;
      if (tally->Floored() != floored) tally->paths_with_floor += 1;
      if (tally->Capped() != capped) tally->paths_with_cap += 1;
    };
  });

  LocalVolEstimate estimate;
  estimate.value.batch_means.reserve(table.size());
  for (const auto& row : table) estimate.value.batch_means.push_back(row[0]);
  const BatchStatistics stats = BatchMeansStatistics(estimate.value.batch_means);
  estimate.value.mean = stats.mean;
  estimate.value.standard_error = stats.standard_error;
  for (const FloorTally& t : tallies) estimate.floors.Merge(t);
  return estimate;
}

}  // namespace aadp::pricer
