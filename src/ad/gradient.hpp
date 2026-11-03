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
#ifndef AADP_AD_GRADIENT_HPP_
#define AADP_AD_GRADIENT_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

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
  // scale of every error bound in tests/ad/), and its size on the tape. Starts and ends with
  // the set-up adjoints clear.
  struct PathResult {
    double value = 0.0;
    std::array<double, kInputs> gradient{};
    std::array<double, kInputs> absolute{};
    std::size_t statements = 0;  // Set-up and path.
    std::size_t operands = 0;
  };
  [[nodiscard]] PathResult Evaluate(const pricer::PathUniforms& u);

  [[nodiscard]] Tape& tape() { return tape_; }
  [[nodiscard]] const std::array<Real, kInputs>& inputs() const { return inputs_; }
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

 private:
  PathGradient path_;
};

}  // namespace aadp::ad

#endif  // AADP_AD_GRADIENT_HPP_
