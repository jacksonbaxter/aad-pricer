#include "ad/second_order.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <utility>
#include <vector>

#include "ad/bounds.hpp"
#include "ad/gradient.hpp"
#include "ad/tape.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/conditional_kernels.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::ad {

namespace {

std::array<double, kInputs> MarketInputs(const pricer::Market<double>& m) {
  return {m.spot, m.sigma, m.r, m.q, m.maturity};
}

template <class Active>
pricer::Market<Active> ActiveMarketOf(const std::array<Active, kInputs>& x) {
  return {.spot = x[0], .sigma = x[1], .r = x[2], .q = x[3], .maturity = x[4]};
}

// Registered in Market order, as the first-order tape registers them, lane k seeded on input
// directions[k].
template <class D>
std::array<SecondReal<D>, kInputs> RegisterSeeded(SecondTape<D>& tape,
                                                  const pricer::Market<double>& m,
                                                  const std::array<std::size_t, D::kLanes>& dirs) {
  const std::array<double, kInputs> x = MarketInputs(m);
  std::array<SecondReal<D>, kInputs> out;
  for (std::size_t i = 0; i < kInputs; ++i) {
    D value(x[i]);
    for (std::size_t k = 0; k < D::kLanes; ++k) value.tangent[k] = dirs[k] == i ? 1.0 : 0.0;
    out[i] = tape.Register(value);
  }
  return out;
}

pricer::Estimate FromBatchMeans(std::vector<double> batch_means) {
  pricer::Estimate estimate;
  const BatchStatistics stats = BatchMeansStatistics(batch_means);
  estimate.mean = stats.mean;
  estimate.standard_error = stats.standard_error;
  estimate.batch_means = std::move(batch_means);
  return estimate;
}

std::expected<void, SecondOrderError> Admit(const pricer::Contract& contract,
                                            const pricer::Market<double>& market,
                                            const pricer::RunConfig& config) {
  const pricer::ContractError contract_error = pricer::Validate(contract, market);
  if (contract_error != pricer::ContractError::kNone) {
    return std::unexpected(SecondOrderError{.price = {.contract = contract_error}});
  }
  const pricer::RunError run_error = pricer::ValidateRun(config);
  if (run_error != pricer::RunError::kNone) {
    return std::unexpected(SecondOrderError{.price = {.run = run_error}});
  }
  const pricer::ConditionalError conditional = pricer::ValidateConditional(contract, market);
  if (conditional != pricer::ConditionalError::kNone) {
    return std::unexpected(SecondOrderError{.price = {}, .conditional = conditional});
  }
  return {};
}

// The conditional kernels read the normal uniforms only.
pricer::PathShape ShapeOf(const pricer::Contract& c) {
  return {.steps = c.steps, .crossing = false};
}

template <std::size_t N>
class HessianPathFn {
 public:
  static constexpr std::size_t kStats = 1 + kInputs + (N * kInputs);

  HessianPathFn(const pricer::Contract& c, const pricer::Market<double>& m,
                const std::array<std::size_t, N>& directions)
      : path_(c, m, directions) {}

  void operator()(const pricer::PathUniforms& u, std::array<double, kStats>& stats) {
    stats = {};
    stats[0] = path_.Accumulate(u);
  }
  void EndBlock(std::array<double, kStats>& sum) {
    const typename SecondOrderPath<HessianDual<N>>::Sums s = path_.SweepSetup();
    for (std::size_t i = 0; i < kInputs; ++i) {
      sum[1 + i] += i == kMaturityInput ? -s.gradient[i] : s.gradient[i];
    }
    for (std::size_t k = 0; k < N; ++k) {
      for (std::size_t i = 0; i < kInputs; ++i) {
        sum[1 + kInputs + (k * kInputs) + i] += s.rows[k][i];
      }
    }
  }

 private:
  SecondOrderPath<HessianDual<N>> path_;
};

class ConditionalGradientPathFn {
 public:
  ConditionalGradientPathFn(const pricer::Contract& c, const pricer::Market<double>& m)
      : path_(c, m) {}

  void operator()(const pricer::PathUniforms& u, std::array<double, kGradientStats>& stats) {
    stats = {};
    stats[0] = path_.Accumulate(u);
  }
  void EndBlock(std::array<double, kGradientStats>& sum) {
    const std::array<double, kInputs> gradient = path_.SweepSetup();
    for (std::size_t i = 0; i < kInputs; ++i) {
      sum[1 + i] += i == kMaturityInput ? -gradient[i] : gradient[i];
    }
  }

 private:
  ConditionalPathGradient path_;
};

}  // namespace

void SecondOrderFail(const char* what) {
  std::fprintf(stderr, "aadp::ad: %s\n", what);
  std::abort();
}

template <class D>
SecondOrderPath<D>::SecondOrderPath(const pricer::Contract& contract,
                                    const pricer::Market<double>& market,
                                    const Directions& directions)
    : contract_(contract),
      scope_(tape_),
      inputs_(RegisterSeeded(tape_, market, directions)),
      model_(ActiveMarketOf(inputs_), contract.steps),
      mark_(tape_.GetPosition()) {}

template <class D>
SecondReal<D> SecondOrderPath<D>::Record(const pricer::PathUniforms& u) {
  return pricer::ConditionalPayoff<SecondReal<D>>(contract_, model_, u);
}

template <class D>
double SecondOrderPath<D>::Accumulate(const pricer::PathUniforms& u) {
  const SecondReal<D> payoff = Record(u);
  tape_.AddAdjoint(payoff, 1.0);
  tape_.SweepBack(tape_.GetPosition(), mark_);
  tape_.Rewind(mark_);
  return payoff.value();
}

template <class D>
SecondOrderPath<D>::Sums SecondOrderPath<D>::ReadInputs() const {
  Sums sums;
  for (std::size_t i = 0; i < kInputs; ++i) {
    const D& adjoint = tape_.Adjoint(inputs_[i]);
    sums.gradient[i] = adjoint.value;
    for (std::size_t k = 0; k < kLanes; ++k) sums.rows[k][i] = adjoint.tangent[k];
  }
  return sums;
}

template <class D>
SecondOrderPath<D>::Sums SecondOrderPath<D>::SweepSetup() {
  tape_.SweepBack(mark_, SecondPosition{});
  const Sums sums = ReadInputs();
  tape_.ClearAdjoints(mark_, SecondPosition{});
  return sums;
}

template <class D>
SecondOrderPath<D>::Sums SecondOrderPath<D>::EvaluateOne(const pricer::PathUniforms& u,
                                                         PathShapeReport& shape) {
  const SecondReal<D> payoff = Record(u);
  const SecondPosition end = tape_.GetPosition();
  shape.value = payoff.value();
  tape_.AddAdjoint(payoff, 1.0);
  // One sweep from the end to the start, keeping every adjoint for the minima below. It forms the
  // same products in the same order as Accumulate and SweepSetup do in two parts, since a
  // statement's adjoint is read once, before anything below it is touched.
  tape_.template SweepBack<true>(end, SecondPosition{});
  const Sums sums = ReadInputs();
  const SecondMemory memory = tape_.Memory();
  shape.statements = memory.statements;
  shape.operands = memory.operands;
  shape.smallest_partial = tape_.SmallestPartial(end, SecondPosition{});
  shape.smallest_adjoint = tape_.SmallestAdjoint(end, SecondPosition{});
  tape_.ClearAdjoints(end, SecondPosition{});
  tape_.Rewind(mark_);
  return sums;
}

template class SecondOrderPath<HessianDual<1>>;
template class SecondOrderPath<HessianDual<2>>;
template class SecondOrderPath<AbsoluteHessianDual<1>>;
template class SecondOrderPath<AbsoluteHessianDual<2>>;

template <std::size_t N>
std::expected<HessianEstimate<N>, SecondOrderError> PriceWithHessian(
    const pricer::Contract& contract, const pricer::Market<double>& market,
    const pricer::RunConfig& config, const std::array<std::size_t, N>& directions) {
  if (auto admitted = Admit(contract, market, config); !admitted) {
    return std::unexpected(admitted.error());
  }
  for (const std::size_t d : directions) {
    if (d >= kInputs) SecondOrderFail("a second-order direction outside the inputs");
  }
  constexpr std::size_t kStats = HessianPathFn<N>::kStats;
  const auto table = pricer::RunBatches<kStats>(
      config, ShapeOf(contract), [&] { return HessianPathFn<N>(contract, market, directions); });
  std::array<std::vector<double>, kStats> columns;
  for (auto& column : columns) column.reserve(table.size());
  for (const auto& row : table) {
    for (std::size_t i = 0; i < kStats; ++i) columns[i].push_back(row[i]);
  }
  HessianEstimate<N> estimate;
  estimate.directions = directions;
  estimate.value = FromBatchMeans(std::move(columns[0]));
  for (std::size_t i = 0; i < kInputs; ++i) {
    estimate.greeks[i] = FromBatchMeans(std::move(columns[1 + i]));
  }
  for (std::size_t k = 0; k < N; ++k) {
    for (std::size_t i = 0; i < kInputs; ++i) {
      estimate.rows[k][i] = FromBatchMeans(std::move(columns[1 + kInputs + (k * kInputs) + i]));
    }
  }
  return estimate;
}

template std::expected<HessianEstimate<1>, SecondOrderError> PriceWithHessian<1>(
    const pricer::Contract&, const pricer::Market<double>&, const pricer::RunConfig&,
    const std::array<std::size_t, 1>&);
template std::expected<HessianEstimate<2>, SecondOrderError> PriceWithHessian<2>(
    const pricer::Contract&, const pricer::Market<double>&, const pricer::RunConfig&,
    const std::array<std::size_t, 2>&);

std::expected<GradientEstimate, SecondOrderError> PriceWithConditionalGradient(
    const pricer::Contract& contract, const pricer::Market<double>& market,
    const pricer::RunConfig& config) {
  if (auto admitted = Admit(contract, market, config); !admitted) {
    return std::unexpected(admitted.error());
  }
  return EstimateFromTable(pricer::RunBatches<kGradientStats>(
      config, ShapeOf(contract), [&] { return ConditionalGradientPathFn(contract, market); }));
}

std::expected<pricer::Estimate, SecondOrderError> PriceConditional(
    const pricer::Contract& contract, const pricer::Market<double>& market,
    const pricer::RunConfig& config) {
  if (auto admitted = Admit(contract, market, config); !admitted) {
    return std::unexpected(admitted.error());
  }
  const pricer::GbmModel<double> model(market, contract.steps);
  const auto table = pricer::RunBatches<1>(config, ShapeOf(contract), [&] {
    return [&](const pricer::PathUniforms& u, std::array<double, 1>& out) {
      out[0] = pricer::ConditionalPayoff<double>(contract, model, u);
    };
  });
  std::vector<double> means;
  means.reserve(table.size());
  for (const auto& row : table) means.push_back(row[0]);
  return FromBatchMeans(std::move(means));
}

ConditionalPathGradient::ConditionalPathGradient(const pricer::Contract& contract,
                                                 const pricer::Market<double>& market)
    : contract_(contract),
      scope_(tape_),
      inputs_({tape_.Register(market.spot), tape_.Register(market.sigma), tape_.Register(market.r),
               tape_.Register(market.q), tape_.Register(market.maturity)}),
      model_(ActiveMarketOf(inputs_), contract.steps),
      mark_(tape_.GetPosition()) {
  tape_.SetSetupEnd(mark_);
}

double ConditionalPathGradient::Accumulate(const pricer::PathUniforms& u) {
  const Real payoff = pricer::ConditionalPayoff<Real>(contract_, model_, u);
  tape_.AddAdjoint(payoff, 1.0);
  tape_.SweepBack(tape_.GetPosition(), mark_);
  tape_.Rewind(mark_);
  return payoff.value();
}

std::array<double, kInputs> ConditionalPathGradient::ReadInputs() const {
  std::array<double, kInputs> out{};
  for (std::size_t i = 0; i < kInputs; ++i) out[i] = tape_.Adjoint(inputs_[i]);
  return out;
}

std::array<double, kInputs> ConditionalPathGradient::SweepSetup() {
  tape_.SweepBack(mark_, Position{});
  const std::array<double, kInputs> gradient = ReadInputs();
  tape_.ClearAdjoints(mark_, Position{});
  return gradient;
}

// PathGradient::Evaluate on the conditional kernel.
PathGradient::PathResult ConditionalPathGradient::Evaluate(const pricer::PathUniforms& u) {
  PathGradient::PathResult result;
  const Real payoff = pricer::ConditionalPayoff<Real>(contract_, model_, u);
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

template <std::size_t N>
HessianCheckPath<N>::HessianCheckPath(const pricer::Contract& contract,
                                      const pricer::Market<double>& market,
                                      const std::array<std::size_t, N>& directions)
    : path_(contract, market, directions), absolute_(contract, market, directions) {}

template <std::size_t N>
HessianCheckPath<N>::Result HessianCheckPath<N>::Evaluate(const pricer::PathUniforms& u) {
  Result r;
  typename SecondOrderPath<HessianDual<N>>::PathShapeReport shape;
  const auto sums = path_.EvaluateOne(u, shape);
  r.value = shape.value;
  r.gradient = sums.gradient;
  r.rows = sums.rows;
  r.statements = shape.statements;
  r.operands = shape.operands;
  r.smallest_partial = shape.smallest_partial;
  typename SecondOrderPath<AbsoluteHessianDual<N>>::PathShapeReport absolute_shape;
  const auto absolute = absolute_.EvaluateOne(u, absolute_shape);
  r.absolute_rows = absolute.rows;
  r.smallest_adjoint = std::min(shape.smallest_adjoint, absolute_shape.smallest_adjoint);
  return r;
}

template class HessianCheckPath<1>;
template class HessianCheckPath<2>;

// The count n, stated before any check ran. A term of row entry (k, i) is a product of reverse
// partials from the output down to some statement s, the tangent lane k of one partial of s, and
// forward partials from that partial's operand down to the seed: forward over reverse is the
// reverse sweep of the tangent program. Its roundings:
//   reverse sweep  at most one product per statement passed (adjoint.t d.v or adjoint.v d.t) and,
//                  at each operand it lands in, the two additions of that lane's update; over the
//                  tape at most T products and 2 O additions;
//   forward lanes  at most 3 roundings per statement passed (the product rule's two products and
//                  one addition; the quotient rule's product, subtraction and division);
//   the partial's own tangent, a Dual expression of at most 6 operations of 3 roundings each
//                  (real.hpp's partials; InverseNormal's 1 / phi(z) is the longest), 18, taken
//                  as 32.
// So n = T + 2 O + 3 T + 32 = 4 T + 2 O + 32, with T statements and O operands on the tape, and
// each computed entry is within gamma_n |terms| of the exact result of the same algorithm on the
// same recorded values (Higham, lemma 3.1), the absolute sweep itself carrying at most gamma_n
// relative error: hence the factor (1 + 2 gamma_n). The model needs every product in the normal
// range, which the callers check: no partial or adjoint lane is nonzero and below 2^-511.
double HessianRoundoffBound(double absolute, std::size_t statements, std::size_t operands) {
  const double n =
      (4.0 * static_cast<double>(statements)) + (2.0 * static_cast<double>(operands)) + 32.0;
  const double gamma = bounds::Gamma(n);
  return gamma * absolute * (1.0 + (2.0 * gamma));
}

}  // namespace aadp::ad
