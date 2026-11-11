#include "quantlib/ours.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "ad/gradient.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/geometric_asian.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/real_ops.hpp"
#include "pricer/runner.hpp"
#include "quantlib/compare.hpp"
#include "simd/lane_gradient.hpp"
#include "simd/simd_pricer.hpp"

namespace aadp::qlcompare {

namespace {

std::string Why(const pricer::PriceError& e) {
  return std::string("pricer refused: ") + pricer::ErrorName(e.contract) +
         (e.run != pricer::RunError::kNone ? " (run configuration)" : "");
}

pricer::Estimate FromBatchMeans(std::vector<double> batch_means) {
  pricer::Estimate estimate;
  estimate.batch_means = std::move(batch_means);
  const BatchStatistics stats = BatchMeansStatistics(estimate.batch_means);
  estimate.mean = stats.mean;
  estimate.standard_error = stats.standard_error;
  return estimate;
}

bool IsGreekRow(Row row) { return row == Row::kR9a || row == Row::kR9b; }

}  // namespace

bool HasLaneForm(const Comparison& comparison) { return comparison.contract.row != Row::kR3; }

std::expected<pricer::Estimate, std::string> PriceControlVariate(
    const pricer::Contract& arithmetic, const pricer::Market<double>& market,
    const pricer::RunConfig& config) {
  if (arithmetic.product != pricer::Product::kAsianArithmetic ||
      arithmetic.type != pricer::OptionType::kCall) {
    return std::unexpected("the control variate prices an arithmetic Asian call");
  }
  const pricer::ContractError error = pricer::Validate(arithmetic, market);
  if (error != pricer::ContractError::kNone) return std::unexpected(pricer::ErrorName(error));
  if (pricer::ValidateRun(config) != pricer::RunError::kNone) {
    return std::unexpected("bad run configuration");
  }
  const MarketParams<double> closed_form_market{.spot = market.spot,
                                                .sigma = market.sigma,
                                                .r = market.r,
                                                .q = market.q,
                                                .maturity = market.maturity};
  const double control = GeometricAsianValue(OptionType::kCall, arithmetic.strike, arithmetic.steps,
                                             arithmetic.include_spot, closed_form_market);
  const pricer::GbmModel<double> model(market, arithmetic.steps);
  const double count =
      static_cast<double>(arithmetic.steps) + (arithmetic.include_spot ? 1.0 : 0.0);
  const double strike = arithmetic.strike;
  const pricer::PathShape shape{.steps = arithmetic.steps, .crossing = false};
  const auto table = pricer::RunBatches<1>(config, shape, [&] {
    return [&](const pricer::PathUniforms& u, std::array<double, 1>& out) {
      double x = model.LogSpot();
      double sum = arithmetic.include_spot ? model.Spot() : 0.0;
      double log_sum = arithmetic.include_spot ? x : 0.0;
      for (std::uint32_t i = 0; i < arithmetic.steps; ++i) {
        const pricer::StepLaw<double> law = model.Law(i, x);
        x = law.mean + law.sd * pricer::InverseNormal(u.normal[i]);
        sum += pricer::Exp(x);
        log_sum += x;
      }
      const double average = sum / count;
      const double geometric = pricer::Exp(log_sum / count);
      const double arithmetic_payoff = average > strike ? average - strike : 0.0;
      const double geometric_payoff = geometric > strike ? geometric - strike : 0.0;
      out[0] = model.Discount() * (arithmetic_payoff - geometric_payoff) + control;
    };
  });
  std::vector<double> means;
  means.reserve(table.size());
  for (const auto& row : table) means.push_back(row[0]);
  return FromBatchMeans(std::move(means));
}

std::expected<pricer::Estimate, std::string> RunOurs(const Comparison& comparison,
                                                     OurVariant variant, std::uint64_t seed,
                                                     std::uint64_t paths, std::uint32_t batches) {
  if (comparison.engine == Engine::kQuantLib) return std::unexpected("not one of ours");
  const pricer::RunConfig config{.seed = seed, .paths = paths, .batches = batches, .threads = 1};
  const pricer::Contract contract = OurContract(comparison.contract, comparison.engine);
  const pricer::Market<double> market = OurMarket(comparison.contract);
  const bool lanes = variant == OurVariant::kLanes;
  if (lanes && !HasLaneForm(comparison)) return std::unexpected("no lane form");

  if (comparison.contract.row == Row::kR3) return PriceControlVariate(contract, market, config);

  if (IsGreekRow(comparison.contract.row)) {
    if (lanes) {
      const auto result = simd::PriceWithGradientLanes(contract, market, config, simd::MaxLanes());
      if (!result) return std::unexpected(Why(result.error().price));
      if (result->fallback_groups != 0) return std::unexpected("lane groups fell back");
      return result->estimate.Of(ad::Greek::kDelta);
    }
    const auto result = ad::PriceWithGradient(contract, market, config);
    if (!result) return std::unexpected(Why(result.error()));
    return result->Of(ad::Greek::kDelta);
  }

  if (lanes) {
    const auto result = simd::PriceLanes(contract, market, config, simd::MaxLanes());
    if (!result) return std::unexpected(Why(result.error().price));
    return *result;
  }
  const auto result = pricer::Price(contract, market, config);
  if (!result) return std::unexpected(Why(result.error()));
  return *result;
}

}  // namespace aadp::qlcompare
