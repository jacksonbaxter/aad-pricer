#include "pricer/bump.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <utility>
#include <vector>

#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::pricer {

namespace {

double& Field(Market<double>& m, Input input) {
  switch (input) {
    case Input::kSpot:
      return m.spot;
    case Input::kSigma:
      return m.sigma;
    case Input::kRate:
      return m.r;
    case Input::kDividend:
      return m.q;
    case Input::kMaturity:
      return m.maturity;
  }
  return m.spot;
}

double FieldOf(Market<double> m, Input input) { return Field(m, input); }

Estimate FromBatchMeans(std::vector<double> batch_means) {
  Estimate estimate;
  const BatchStatistics stats = BatchMeansStatistics(batch_means);
  estimate.mean = stats.mean;
  estimate.standard_error = stats.standard_error;
  estimate.batch_means = std::move(batch_means);
  return estimate;
}

double Mean(const std::vector<double>& values) {
  double sum = 0.0;
  for (const double x : values) sum += x;
  return sum / static_cast<double>(values.size());
}

// The batch-wise quotient (up - down) / denominator, negated for theta.
Estimate Quotient(const Estimate& up, const Estimate& down, double denominator, Input input) {
  std::vector<double> batch_means(up.batch_means.size());
  const double sign = input == Input::kMaturity ? -1.0 : 1.0;
  for (std::size_t b = 0; b < batch_means.size(); ++b) {
    batch_means[b] = sign * ((up.batch_means[b] - down.batch_means[b]) / denominator);
  }
  return FromBatchMeans(std::move(batch_means));
}

}  // namespace

Market<double> Bumped(const Market<double>& market, Input input, double h) {
  Market<double> out = market;
  Field(out, input) += h;
  return out;
}

Contract ContractAt(const Contract& contract, const Market<double>& base,
                    const Market<double>& bumped) {
  Contract out = contract;
  if (contract.product == Product::kBarrier && contract.barrier.treatment == Treatment::kSmoothed &&
      bumped.spot != base.spot) {
    out.barrier.smoothing_width = contract.barrier.smoothing_width * (base.spot / bumped.spot);
  }
  return out;
}

std::expected<Revaluation, PriceError> Revalue(const Contract& contract,
                                               const Market<double>& market,
                                               const RunConfig& config) {
  const ContractError contract_error = Validate(contract, market);
  if (contract_error != ContractError::kNone) {
    return std::unexpected(PriceError{.contract = contract_error});
  }
  const RunError run_error = ValidateRun(config);
  if (run_error != RunError::kNone) return std::unexpected(PriceError{.run = run_error});

  const GbmModel<double> model(market, contract.steps);
  const PathShape shape{.steps = contract.steps, .crossing = NeedsCrossingUniforms(contract)};
  // Statistic 0 is Price()'s, summed in the same order, so its batch means are the same bits.
  const auto table = RunBatches<2>(config, shape, [&] {
    return [&](const PathUniforms& u, std::array<double, 2>& out) {
      out[0] = DiscountedPayoff<double>(contract, model, u);
      out[1] = std::fabs(out[0]);
    };
  });
  std::vector<double> values;
  std::vector<double> absolutes;
  values.reserve(table.size());
  absolutes.reserve(table.size());
  for (const auto& row : table) {
    values.push_back(row[0]);
    absolutes.push_back(row[1]);
  }
  return Revaluation{.value = FromBatchMeans(std::move(values)), .mean_absolute = Mean(absolutes)};
}

std::expected<BumpEstimate, BumpError> PriceWithBumps(const Contract& contract,
                                                      const Market<double>& market,
                                                      const RunConfig& config, BumpScheme scheme,
                                                      const BumpSteps& steps) {
  auto base = Revalue(contract, market, config);
  if (!base) return std::unexpected(BumpError{.price = base.error()});
  BumpEstimate out;
  out.pricings = 1;
  for (std::size_t i = 0; i < kInputCount; ++i) {
    const auto input = static_cast<Input>(i);
    const Market<double> up_market = Bumped(market, input, steps[i]);
    auto up = Revalue(ContractAt(contract, market, up_market), up_market, config);
    if (!up) return std::unexpected(BumpError{.price = up.error(), .bumped = true, .input = input});
    out.pricings += 1;
    if (scheme == BumpScheme::kCentral) {
      const Market<double> down_market = Bumped(market, input, -steps[i]);
      auto down = Revalue(ContractAt(contract, market, down_market), down_market, config);
      if (!down) {
        return std::unexpected(BumpError{.price = down.error(), .bumped = true, .input = input});
      }
      out.pricings += 1;
      out.denominator[i] = FieldOf(up_market, input) - FieldOf(down_market, input);
      out.greeks[i] = Quotient(up->value, down->value, out.denominator[i], input);
      out.mean_absolute_down[i] = down->mean_absolute;
    } else {
      out.denominator[i] = FieldOf(up_market, input) - FieldOf(market, input);
      out.greeks[i] = Quotient(up->value, base->value, out.denominator[i], input);
      out.mean_absolute_down[i] = base->mean_absolute;
    }
    out.mean_absolute_up[i] = up->mean_absolute;
  }
  out.value = std::move(base->value);
  out.mean_absolute = base->mean_absolute;
  return out;
}

}  // namespace aadp::pricer
