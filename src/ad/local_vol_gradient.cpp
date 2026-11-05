#include "ad/local_vol_gradient.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include "ad/tape.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "pricer/substep_kernels.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::ad {

namespace {

std::vector<Real> RegisterInputs(Tape& tape, std::span<const svi::RawSvi<double>> params) {
  std::vector<Real> inputs;
  inputs.reserve(params.size() * kParametersPerSlice);
  for (const svi::RawSvi<double>& p : params) {
    for (const double x : {p.a, p.b, p.rho, p.m, p.sigma}) inputs.push_back(tape.Register(x));
  }
  return inputs;
}

pricer::LocalVolModel<Real> BuildModel(const pricer::LocalVolContract& c,
                                       std::span<const svi::SliceTerms> terms,
                                       const std::vector<Real>& x, const svi::LocalVolRule& rule) {
  std::vector<svi::RawSvi<Real>> slices;
  slices.reserve(terms.size());
  for (std::size_t j = 0; j < terms.size(); ++j) {
    const std::size_t i = kParametersPerSlice * j;
    slices.push_back({.a = x[i], .b = x[i + 1], .rho = x[i + 2], .m = x[i + 3], .sigma = x[i + 4]});
  }
  return {svi::SviSurface<Real>(terms, slices), c.maturity, c.contract.steps * c.substeps, rule};
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

LocalVolPathGradient::LocalVolPathGradient(const pricer::LocalVolContract& c,
                                           std::span<const svi::SliceTerms> terms,
                                           std::span<const svi::RawSvi<double>> params,
                                           const svi::LocalVolRule& rule)
    : contract_(c.contract),
      substeps_(c.substeps),
      scope_(tape_),
      inputs_(RegisterInputs(tape_, params)),
      model_(BuildModel(c, terms, inputs_, rule)),
      mark_(tape_.GetPosition()) {
  tape_.SetSetupEnd(mark_);
}

Real LocalVolPathGradient::Record(const pricer::PathUniforms& u) {
  return pricer::EvaluateSubstepped<Real>(contract_, substeps_, model_, u).value;
}

double LocalVolPathGradient::Accumulate(const pricer::PathUniforms& u) {
  const Real payoff = Record(u);
  tape_.AddAdjoint(payoff, 1.0);
  tape_.SweepBack(tape_.GetPosition(), mark_);
  tape_.Rewind(mark_);
  return payoff.value();
}

void LocalVolPathGradient::ReadInputs(std::span<double> out) const {
  for (std::size_t i = 0; i < inputs_.size(); ++i) out[i] = tape_.Adjoint(inputs_[i]);
}

std::array<double, kMaxSurfaceInputs> LocalVolPathGradient::SweepSetup() {
  tape_.SweepBack(mark_, Position{});
  std::array<double, kMaxSurfaceInputs> gradient{};
  ReadInputs(gradient);
  tape_.ClearAdjoints(mark_, Position{});
  return gradient;
}

LocalVolPathGradient::PathResult LocalVolPathGradient::Evaluate(const pricer::PathUniforms& u) {
  // A check may record a path more than once, so it is not counted.
  model_.SetTally(nullptr);
  PathResult result;
  const std::size_t n = inputs_.size();
  result.gradient.resize(n);
  result.absolute.resize(n);
  result.operand_sums.resize(n);
  const Real payoff = Record(u);
  const Position end = tape_.GetPosition();
  result.value = payoff.value();
  tape_.AddAdjoint(payoff, 1.0);
  tape_.SweepBack(end, mark_);
  const std::array<double, kMaxSurfaceInputs> gradient = SweepSetup();
  for (std::size_t i = 0; i < n; ++i) result.gradient[i] = gradient[i];
  tape_.AddAdjoint(payoff, 1.0);
  tape_.SweepBackAbsolute(end, Position{});
  ReadInputs(result.absolute);
  for (std::size_t i = 0; i < n; ++i) {
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
  model_.SetTally(tally_);
  return result;
}

void LocalVolGradientPathFn::operator()(const pricer::PathUniforms& u,
                                        std::array<double, kLocalVolGradientStats>& stats) {
  stats = {};
  const std::uint64_t floored = tally_->Floored();
  const std::uint64_t capped = tally_->Capped();
  stats[0] = path_.Accumulate(u);
  tally_->paths += 1;
  if (tally_->Floored() != floored) tally_->paths_with_floor += 1;
  if (tally_->Capped() != capped) tally_->paths_with_cap += 1;
}

void LocalVolGradientPathFn::EndBlock(std::array<double, kLocalVolGradientStats>& sum) {
  const std::array<double, kMaxSurfaceInputs> gradient = path_.SweepSetup();
  for (std::size_t i = 0; i < path_.Inputs(); ++i) sum[1 + i] += gradient[i];
}

std::expected<LocalVolGradientEstimate, pricer::LocalVolFailure> PriceLocalVolWithGradient(
    const pricer::LocalVolContract& c, std::span<const svi::SliceTerms> terms,
    std::span<const svi::RawSvi<double>> params, const pricer::RunConfig& config,
    const svi::LocalVolRule& rule) {
  const pricer::LocalVolFailure failure = pricer::ValidateLocalVol(c, terms, params, rule);
  if (failure.code != pricer::LocalVolError::kNone) return std::unexpected(failure);
  const pricer::RunError run = pricer::ValidateRun(config);
  if (run != pricer::RunError::kNone) {
    return std::unexpected(
        pricer::LocalVolFailure{.code = pricer::LocalVolError::kRun, .run = run});
  }
  std::vector<pricer::FloorTally> tallies(config.threads);
  std::atomic<std::size_t> next{0};
  const pricer::PathShape shape{.steps = c.contract.steps * c.substeps,
                                .crossing = pricer::NeedsCrossingUniforms(c.contract)};
  const auto table = pricer::RunBatches<kLocalVolGradientStats>(config, shape, [&] {
    return LocalVolGradientPathFn(c, terms, params, rule,
                                  &tallies[next.fetch_add(1, std::memory_order_relaxed)]);
  });

  const std::size_t n = params.size() * kParametersPerSlice;
  std::vector<std::vector<double>> columns(1 + n);
  for (auto& column : columns) column.reserve(table.size());
  for (const auto& row : table) {
    for (std::size_t i = 0; i <= n; ++i) columns[i].push_back(row[i]);
  }
  LocalVolGradientEstimate estimate;
  estimate.value = FromBatchMeans(std::move(columns[0]));
  estimate.gradient.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    estimate.gradient.push_back(FromBatchMeans(std::move(columns[1 + i])));
  }
  for (const pricer::FloorTally& t : tallies) estimate.floors.Merge(t);
  return estimate;
}

}  // namespace aadp::ad
