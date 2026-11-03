#include "pricer/pricer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <vector>

#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/runner.hpp"

namespace aadp::pricer {

namespace {

// False on NaN.
bool InRange(double x, double lo, double hi) { return x >= lo && x <= hi; }

bool IsAsian(Product product) {
  return product == Product::kAsianArithmetic || product == Product::kAsianGeometric;
}

ContractError ValidateBarrier(const Barrier& b, double spot) {
  if (static_cast<std::uint8_t>(b.direction) > 1 || static_cast<std::uint8_t>(b.knock) > 1 ||
      static_cast<std::uint8_t>(b.monitoring) > 1 ||
      static_cast<std::uint8_t>(b.treatment) >= kTreatmentCount) {
    return ContractError::kBarrierField;
  }
  // The spot must be on the live side: a contract already knocked is not this pricer's to price.
  if (!InRange(b.level, kMinPrice, kMaxPrice)) return ContractError::kBarrierLevel;
  if (b.direction == Direction::kDown ? !(b.level < spot) : !(b.level > spot)) {
    return ContractError::kBarrierLevel;
  }
  const bool continuous = b.monitoring == Monitoring::kContinuous;
  if (!continuous && b.treatment == Treatment::kBridge) return ContractError::kBridgeOnDiscrete;
  if (continuous && b.treatment == Treatment::kOneStep) {
    return ContractError::kOneStepOnContinuous;
  }
  if (continuous && b.treatment == Treatment::kSmoothed) {
    return ContractError::kSmoothedOnContinuous;
  }
  if (b.treatment == Treatment::kSmoothed &&
      !InRange(b.smoothing_width, kMinSmoothingWidth, kMaxSmoothingWidth)) {
    return ContractError::kSmoothingWidth;
  }
  return ContractError::kNone;
}

double Magnitude(double x) { return x < 0.0 ? -x : x; }

// Every log price a path can reach stays within kMaxLogExcursion of zero, and so does r T. Per
// step, an unconditioned draw moves log S by at most |drift| + 8.3 sd, and a one-step conditioned
// draw by at most 2 |drift| + 8.6 sd, because the path it starts from is on the live side. So
// S, sums of up to kMaxSteps of S, the discount and their products all stay below DBL_MAX.
bool WithinRange(const Contract& c, const Market<double>& m) {
  double log_scale = std::max(Magnitude(math::log(m.spot)), Magnitude(math::log(c.strike)));
  if (c.product == Product::kBarrier) {
    log_scale = std::max(log_scale, Magnitude(math::log(c.barrier.level)));
  }
  const double drift = Magnitude(m.r - m.q - 0.5 * m.sigma * m.sigma) * m.maturity;
  const double diffusion =
      kMaxDrawMagnitude * m.sigma * math::sqrt(m.maturity * static_cast<double>(c.steps));
  return log_scale + 2.0 * drift + diffusion <= kMaxLogExcursion &&
         Magnitude(m.r) * m.maturity <= kMaxLogExcursion;
}

}  // namespace

ContractError Validate(const Contract& c, const Market<double>& m) {
  if (static_cast<std::uint8_t>(c.product) >= kProductCount) return ContractError::kProduct;
  if (static_cast<std::uint8_t>(c.type) > 1) return ContractError::kOptionType;
  if (!InRange(m.spot, kMinPrice, kMaxPrice)) return ContractError::kSpot;
  if (!InRange(c.strike, kMinPrice, kMaxPrice)) return ContractError::kStrike;
  if (!InRange(m.sigma, kMinVolatility, kMaxVolatility)) return ContractError::kVolatility;
  if (!InRange(m.maturity, kMinMaturity, kMaxMaturity)) return ContractError::kMaturity;
  if (!InRange(m.r, -kMaxRate, kMaxRate) || !InRange(m.q, -kMaxRate, kMaxRate)) {
    return ContractError::kRate;
  }
  if (c.steps < 1 || c.steps > kMaxSteps) return ContractError::kSteps;
  if (c.include_spot && !IsAsian(c.product)) return ContractError::kIncludeSpot;
  if (c.product == Product::kBarrier) {
    const ContractError barrier = ValidateBarrier(c.barrier, m.spot);
    if (barrier != ContractError::kNone) return barrier;
  }
  if (!WithinRange(c, m)) return ContractError::kRange;
  return ContractError::kNone;
}

const char* ErrorName(ContractError error) {
  switch (error) {
    case ContractError::kNone:
      return "none";
    case ContractError::kProduct:
      return "unknown product";
    case ContractError::kOptionType:
      return "unknown option type";
    case ContractError::kSpot:
      return "spot out of range";
    case ContractError::kStrike:
      return "strike out of range";
    case ContractError::kVolatility:
      return "volatility out of range";
    case ContractError::kMaturity:
      return "maturity out of range";
    case ContractError::kRate:
      return "rate or dividend yield out of range";
    case ContractError::kSteps:
      return "steps out of range";
    case ContractError::kIncludeSpot:
      return "include_spot on a product that does not average";
    case ContractError::kBarrierField:
      return "unknown barrier direction, knock, monitoring or treatment";
    case ContractError::kBarrierLevel:
      return "barrier out of range or not on the far side of the spot";
    case ContractError::kSmoothingWidth:
      return "smoothing width out of range";
    case ContractError::kBridgeOnDiscrete:
      return "the bridge prices a continuous barrier, not a discrete one";
    case ContractError::kOneStepOnContinuous:
      return "one-step survival prices a discrete barrier, not a continuous one";
    case ContractError::kSmoothedOnContinuous:
      return "smoothing applies at monitoring dates, so only to a discrete barrier";
    case ContractError::kRange:
      return "parameters could carry a path outside the finite range";
  }
  return "unknown error";
}

std::expected<Estimate, PriceError> Price(const Contract& contract, const Market<double>& market,
                                          const RunConfig& config) {
  const ContractError contract_error = Validate(contract, market);
  if (contract_error != ContractError::kNone) {
    return std::unexpected(PriceError{.contract = contract_error});
  }
  const RunError run_error = ValidateRun(config);
  if (run_error != RunError::kNone) return std::unexpected(PriceError{.run = run_error});

  const GbmModel<double> model(market, contract.steps);
  const PathShape shape{.steps = contract.steps, .crossing = NeedsCrossingUniforms(contract)};
  const auto table = RunBatches<1>(config, shape, [&] {
    return [&](const PathUniforms& u, std::array<double, 1>& out) {
      out[0] = DiscountedPayoff<double>(contract, model, u);
    };
  });
  Estimate estimate;
  estimate.batch_means.reserve(table.size());
  for (const auto& row : table) estimate.batch_means.push_back(row[0]);
  const BatchStatistics stats = BatchMeansStatistics(estimate.batch_means);
  estimate.mean = stats.mean;
  estimate.standard_error = stats.standard_error;
  return estimate;
}

}  // namespace aadp::pricer
