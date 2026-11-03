#include "ad/gradient.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <utility>
#include <vector>

#include "ad/tape.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::ad {

namespace {

std::array<Real, kInputs> RegisterInputs(Tape& tape, const pricer::Market<double>& m) {
  return {tape.Register(m.spot), tape.Register(m.sigma), tape.Register(m.r), tape.Register(m.q),
          tape.Register(m.maturity)};
}

pricer::Market<Real> ActiveMarket(const std::array<Real, kInputs>& x) {
  return {.spot = x[0], .sigma = x[1], .r = x[2], .q = x[3], .maturity = x[4]};
}

pricer::Estimate FromBatchMeans(std::vector<double> batch_means) {
  pricer::Estimate estimate;
  const BatchStatistics stats = BatchMeansStatistics(batch_means);
  estimate.mean = stats.mean;
  estimate.standard_error = stats.standard_error;
  estimate.batch_means = std::move(batch_means);
  return estimate;
}

}  // namespace

PathGradient::PathGradient(const pricer::Contract& contract, const pricer::Market<double>& market)
    : contract_(contract),
      scope_(tape_),
      inputs_(RegisterInputs(tape_, market)),
      model_(ActiveMarket(inputs_), contract.steps),
      mark_(tape_.GetPosition()) {}

Real PathGradient::Record(const pricer::PathUniforms& u) {
  return pricer::DiscountedPayoff<Real>(contract_, model_, u);
}

double PathGradient::Accumulate(const pricer::PathUniforms& u) {
  const Real payoff = Record(u);
  tape_.AddAdjoint(payoff, 1.0);
  tape_.SweepBack(tape_.GetPosition(), mark_);
  tape_.Rewind(mark_);
  return payoff.value();
}

std::array<double, kInputs> PathGradient::ReadInputs() const {
  std::array<double, kInputs> out{};
  for (std::size_t i = 0; i < kInputs; ++i) out[i] = tape_.Adjoint(inputs_[i]);
  return out;
}

std::array<double, kInputs> PathGradient::SweepSetup() {
  tape_.SweepBack(mark_, Position{});
  const std::array<double, kInputs> gradient = ReadInputs();
  tape_.ClearAdjoints(mark_, Position{});
  return gradient;
}

PathGradient::PathResult PathGradient::Evaluate(const pricer::PathUniforms& u) {
  PathResult result;
  const Real payoff = Record(u);
  const Position end = tape_.GetPosition();
  result.value = payoff.value();
  tape_.AddAdjoint(payoff, 1.0);
  tape_.SweepBack(end, mark_);
  result.gradient = SweepSetup();
  tape_.AddAdjoint(payoff, 1.0);
  tape_.SweepBackAbsolute(end, Position{});
  result.absolute = ReadInputs();
  for (std::size_t i = 0; i < kInputs; ++i) {
    tape_.ClearTangents();
    tape_.SetTangent(inputs_[i], 1.0);
    tape_.SweepForwardAbsolute(Position{});
    const Tape::OperandSums sums = tape_.SumOverOperands(end, Position{});
    result.operand_sums[i] = sums.adjoints + sums.tangents;
  }
  tape_.ClearTangents();
  tape_.ClearAdjoints(end, Position{});
  const MemoryReport memory = tape_.Memory();
  result.statements = memory.statements;
  result.operands = memory.operands;
  tape_.Rewind(mark_);
  return result;
}

void GradientPathFn::EndBlock(std::array<double, kGradientStats>& sum) {
  const std::array<double, kInputs> gradient = path_.SweepSetup();
  for (std::size_t i = 0; i < kInputs; ++i) {
    sum[1 + i] += i == kMaturityInput ? -gradient[i] : gradient[i];
  }
}

std::expected<GradientEstimate, pricer::PriceError> PriceWithGradient(
    const pricer::Contract& contract, const pricer::Market<double>& market,
    const pricer::RunConfig& config) {
  const pricer::ContractError contract_error = pricer::Validate(contract, market);
  if (contract_error != pricer::ContractError::kNone) {
    return std::unexpected(pricer::PriceError{.contract = contract_error});
  }
  const pricer::RunError run_error = pricer::ValidateRun(config);
  if (run_error != pricer::RunError::kNone) {
    return std::unexpected(pricer::PriceError{.run = run_error});
  }
  const pricer::PathShape shape{.steps = contract.steps,
                                .crossing = pricer::NeedsCrossingUniforms(contract)};
  const auto table = pricer::RunBatches<kGradientStats>(
      config, shape, [&] { return GradientPathFn(contract, market); });
  std::array<std::vector<double>, kGradientStats> columns;
  for (auto& column : columns) column.reserve(table.size());
  for (const auto& row : table) {
    for (std::size_t i = 0; i < kGradientStats; ++i) columns[i].push_back(row[i]);
  }
  GradientEstimate estimate;
  estimate.value = FromBatchMeans(std::move(columns[0]));
  for (std::size_t i = 0; i < kInputs; ++i) {
    estimate.greeks[i] = FromBatchMeans(std::move(columns[1 + i]));
  }
  return estimate;
}

}  // namespace aadp::ad
