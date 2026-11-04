// Price and every first-order Greek from one backward pass per path.
//
//   PriceWithGradient(contract, market, {seed, paths, batches, threads})
//       -> GradientEstimate {value, greeks[delta, vega, rho, dividend rho, theta]}, each an
//          Estimate {mean, standard_error, batch_means}, or the reason it refused
//
// Theta is -dV/dT. The value is Price()'s, bit for bit: the same paths, the same kernel values
// and the same sums.
//
// How a worker spends its tape (PathGradient). The five market inputs are registered and the
// model is built from them once: that is the set-up, and the tape is marked after it. Each path
// is recorded after the mark, swept back to the mark from a unit seed on its payoff, so its
// adjoints add into the set-up's, and rewound. At the end of each block of paths (runner.hpp,
// EndBlock), one sweep from the mark to the start turns the set-up adjoints into the block's sum
// of dV/d(input), and clears them. So set-up costs one sweep per block, not one per path, and a
// larger set-up (the SVI surface) does not change the per-path cost (NOTES.md, "AAD"). Batch
// statistics come from the block sums as they do for the price.
//
// Threads. One tape per worker, each recording its own set-up. RunBatches calls make_path_fn on
// each worker, so a GradientPathFn, its tape and its TapeScope are built, used and destroyed on
// one thread and no tape is ever read by another. A shared read-only set-up was the other
// choice. It would save recording the set-up's 18 statements on each worker, against 1,263 for
// one 252-step Asian path, but path operands would then point into a second tape, and the
// set-up adjoints, which every block writes, would need one copy per worker anyway. Recording it
// per worker costs threads times the set-up, whatever N is, and since every worker records the
// same operations on the same doubles, the set-up's partials are the same bits on every worker.
//
// The set-up is swept once per block (EndBlock), on the worker that ran the block: never per
// path, and the same number of times for any thread count. Not once per worker: a worker's
// blocks belong to several batches, and which ones depends on scheduling, so one sweep over all
// of them could not give per-batch Greeks, and their SE, that do not depend on the thread count,
// unless it kept one set-up adjoint vector per block, which costs what the block's sweep does.
// Per block it is 1 / 1024 of a set-up sweep per path. tests/ad/thread_test.cpp counts the
// set-up statements swept.
//
// EVIDENCE B11, B13 and B14 (BENCHMARKS.md section 14):
//   B11  met, with no tolerance at all: the value, every Greek and every batch mean, hence each
//        SE, are the same bits for 1, 2, 3 and 8 threads, and equal to a plain loop with no
//        threads (tests/ad/thread_test.cpp). No skip-ahead is needed, since a path's uniforms
//        depend only on (seed, path, step) (pricer/draws.hpp).
//   B13  not met here: it is a timing, the slot in tape.hpp with and without thread_local, and
//        timed benchmarks are stage 3's. Every recorded operation reads the slot once.
//   B14  met: each block's sums, Greeks included, go to that block's own slot, and the slots are
//        merged in block order after every worker has joined (pricer/runner.hpp). The two
//        reference designs, Stan's accumulate-into-parent and Savine's sum over per-worker
//        models (EVIDENCE B14), group the sum by worker, so its bits can change with the thread
//        count. Here nothing depends on which worker ran which block.
#ifndef AADP_AD_GRADIENT_HPP_
#define AADP_AD_GRADIENT_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

#include "ad/real.hpp"
#include "ad/tape.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::ad {

// The market inputs in Market order, and the Greeks they give.
inline constexpr std::size_t kInputs = 5;
enum class Greek : std::uint8_t { kDelta, kVega, kRho, kDividendRho, kTheta };
inline constexpr std::size_t kMaturityInput = 4;

struct GradientEstimate {
  pricer::Estimate value;
  std::array<pricer::Estimate, kInputs> greeks;

  [[nodiscard]] const pricer::Estimate& Of(Greek greek) const {
    return greeks[static_cast<std::size_t>(greek)];
  }
};

std::expected<GradientEstimate, pricer::PriceError> PriceWithGradient(
    const pricer::Contract& contract, const pricer::Market<double>& market,
    const pricer::RunConfig& config);

// One worker's tape for one contract. It activates its tape on the constructing thread, so it
// must be built, used and destroyed there, with no other tape active on it.
class PathGradient {
 public:
  // Precondition: Validate(contract, market) passed.
  PathGradient(const pricer::Contract& contract, const pricer::Market<double>& market);

  // Records one path after the mark, sweeps it back to the mark and rewinds. Returns the
  // discounted payoff.
  double Accumulate(const pricer::PathUniforms& u);
  // The set-up sweep: dV/d(input) summed over every path accumulated since the last call.
  [[nodiscard]] std::array<double, kInputs> SweepSetup();

  // One path alone, for checks: its value, its gradient, the same sweep on |partials| (the
  // scale of every error bound in tests/ad/), its underflow weights, and its size on the tape.
  // Starts and ends with the set-up adjoints clear.
  struct PathResult {
    double value = 0.0;
    std::array<double, kInputs> gradient{};
    std::array<double, kInputs> absolute{};
    // Per input, over every operand on the tape, the statement's absolute adjoint plus the
    // operand's absolute tangent seeded on that input: the weight of the underflow term in
    // src/ad/bounds.hpp.
    std::array<double, kInputs> operand_sums{};
    std::size_t statements = 0;  // Set-up and path.
    std::size_t operands = 0;
  };
  [[nodiscard]] PathResult Evaluate(const pricer::PathUniforms& u);

  [[nodiscard]] Tape& tape() { return tape_; }
  [[nodiscard]] const Tape& tape() const { return tape_; }
  [[nodiscard]] const std::array<Real, kInputs>& inputs() const { return inputs_; }
  // The set-up: the model built on the registered inputs.
  [[nodiscard]] const pricer::GbmModel<Real>& model() const { return model_; }
  [[nodiscard]] Position mark() const { return mark_; }
  // Records one path after the mark and returns its payoff, leaving it on the tape. The caller
  // sweeps or reads it, then calls Rewind.
  [[nodiscard]] Real Record(const pricer::PathUniforms& u);
  void Rewind() { tape_.Rewind(mark_); }

 private:
  [[nodiscard]] std::array<double, kInputs> ReadInputs() const;

  pricer::Contract contract_;
  Tape tape_;
  TapeScope scope_;
  std::array<Real, kInputs> inputs_;
  pricer::GbmModel<Real> model_;
  Position mark_;
};

// The RunBatches path function: statistic 0 is the payoff, 1 to 5 the Greeks, which only
// EndBlock fills.
inline constexpr std::size_t kGradientStats = 1 + kInputs;

class GradientPathFn {
 public:
  GradientPathFn(const pricer::Contract& contract, const pricer::Market<double>& market)
      : path_(contract, market) {}

  void operator()(const pricer::PathUniforms& u, std::array<double, kGradientStats>& stats) {
    stats = {};
    stats[0] = path_.Accumulate(u);
  }
  void EndBlock(std::array<double, kGradientStats>& sum);

  [[nodiscard]] const PathGradient& path() const { return path_; }

 private:
  PathGradient path_;
};

// Batch means of RunBatches<kGradientStats> as estimates: the value, then the Greeks.
GradientEstimate EstimateFromTable(const std::vector<std::array<double, kGradientStats>>& table);

}  // namespace aadp::ad

#endif  // AADP_AD_GRADIENT_HPP_
