// The local-volatility price and its derivative with respect to every raw SVI parameter of
// every slice, from one backward pass per path (NOTES.md, "Stage 7a").
//
//   PriceLocalVolWithGradient(contract, terms, params, config, rule)
//       -> {value, gradient[5 j + i] for slice j and parameter i in (a, b, rho, m, sigma),
//           FloorTally}, or the reason it refused
//
// The value is PriceLocalVol's, bit for bit. The derivative holds the slices' F, D, T and the spot
// fixed: it is the partial derivative of the Monte Carlo price on the surface as fitted, with no
// refit. Floored and capped local variances are constants and carry none.
//
// The tape is spent as gradient.hpp spends it. The set-up registers the 5 n parameters and
// records each slice's sigma^2 and b sigma^2 (svi/surface.hpp, SliceSetup); everything else the
// model needs per step is a passive double. The tape is marked after the set-up, each path is
// recorded, swept back to the mark and rewound, and the set-up is swept once per block (runner.hpp,
// EndBlock). One tape per worker, each recording its own set-up, so the batch means and their SE
// do not depend on the thread count.
#ifndef AADP_AD_LOCAL_VOL_GRADIENT_HPP_
#define AADP_AD_LOCAL_VOL_GRADIENT_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "ad/real.hpp"
#include "ad/tape.hpp"
#include "pricer/draws.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::ad {

inline constexpr std::size_t kParametersPerSlice = 5;
inline constexpr std::size_t kMaxSurfaceInputs = kParametersPerSlice * svi::kMaxSurfaceSlices;

struct LocalVolGradientEstimate {
  pricer::Estimate value;
  std::vector<pricer::Estimate> gradient;  // 5 per slice, in slice order, (a, b, rho, m, sigma)
  pricer::FloorTally floors;
};

[[nodiscard]] std::expected<LocalVolGradientEstimate, pricer::LocalVolFailure>
PriceLocalVolWithGradient(const pricer::LocalVolContract& c, std::span<const svi::SliceTerms> terms,
                          std::span<const svi::RawSvi<double>> params,
                          const pricer::RunConfig& config, const svi::LocalVolRule& rule = {});

// One worker's tape for one contract on one surface. It activates its tape on the constructing
// thread, so it must be built, used and destroyed there, with no other tape active on it.
class LocalVolPathGradient {
 public:
  // Precondition: ValidateLocalVol passed.
  LocalVolPathGradient(const pricer::LocalVolContract& c, std::span<const svi::SliceTerms> terms,
                       std::span<const svi::RawSvi<double>> params, const svi::LocalVolRule& rule);

  [[nodiscard]] std::size_t Inputs() const { return inputs_.size(); }
  void SetTally(pricer::FloorTally* tally) {
    tally_ = tally;
    model_.SetTally(tally);
  }

  // Records one path after the mark, sweeps it back to the mark and rewinds. Returns the
  // discounted payoff.
  double Accumulate(const pricer::PathUniforms& u);
  // The set-up sweep: d price / d parameter summed over every path since the last call, in
  // the first Inputs() entries.
  [[nodiscard]] std::array<double, kMaxSurfaceInputs> SweepSetup();

  // One path alone, for checks, as gradient.hpp's PathGradient::Evaluate: its value, gradient,
  // absolute gradient, underflow weights and size on the tape. Starts and ends with the set-up
  // adjoints clear.
  struct PathResult {
    double value = 0.0;
    std::vector<double> gradient;
    std::vector<double> absolute;
    std::vector<double> operand_sums;
    std::size_t statements = 0;
    std::size_t operands = 0;
  };
  [[nodiscard]] PathResult Evaluate(const pricer::PathUniforms& u);

  [[nodiscard]] Tape& tape() { return tape_; }
  [[nodiscard]] Position mark() const { return mark_; }
  [[nodiscard]] Real Record(const pricer::PathUniforms& u);

 private:
  void ReadInputs(std::span<double> out) const;

  pricer::Contract contract_;
  std::uint32_t substeps_;
  Tape tape_;
  TapeScope scope_;
  std::vector<Real> inputs_;
  pricer::LocalVolModel<Real> model_;
  Position mark_;
  pricer::FloorTally* tally_ = nullptr;
};

// RunBatches' path function: statistic 0 is the payoff, 1 + i parameter i, which only EndBlock
// fills.
inline constexpr std::size_t kLocalVolGradientStats = 1 + kMaxSurfaceInputs;

class LocalVolGradientPathFn {
 public:
  LocalVolGradientPathFn(const pricer::LocalVolContract& c, std::span<const svi::SliceTerms> terms,
                         std::span<const svi::RawSvi<double>> params, const svi::LocalVolRule& rule,
                         pricer::FloorTally* tally)
      : path_(c, terms, params, rule), tally_(tally) {
    path_.SetTally(tally);
  }

  void operator()(const pricer::PathUniforms& u, std::array<double, kLocalVolGradientStats>& stats);
  void EndBlock(std::array<double, kLocalVolGradientStats>& sum);

 private:
  LocalVolPathGradient path_;
  pricer::FloorTally* tally_;
};

}  // namespace aadp::ad

#endif  // AADP_AD_LOCAL_VOL_GRADIENT_HPP_
