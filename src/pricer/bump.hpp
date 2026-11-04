// Bump and revalue: the five first-order Greeks by finite differences of whole pricings.
//
//   PriceWithBumps(contract, market, {seed, paths, batches, threads}, scheme, steps)
//       -> BumpEstimate {value, greeks[delta, vega, rho, dividend rho, theta], ...}
//
// Each bumped market is priced in full, as a revaluation, with the same RunConfig, so every
// pricing sees the same uniforms (common random numbers), the same path count, the same kernels
// and the same build. Central differences cost 2 N + 1 pricings and forward (one-sided)
// differences N + 1, with N = 5 inputs: BENCHMARKS.md AAD-7's formula (Capriotti, Jiang and
// Macrina 2017, section 4.2). Nothing is shared between the pricings, not even the uniforms,
// because a revaluation shares nothing.
//
// A Greek's batch means are the batch-wise difference quotients, (V+_b - V-_b) / (x+ - x-), so
// its standard error is that of the difference estimator on the same paths, and its mean is the
// difference of the two means. The denominator is the realised step, the difference of the two
// bumped doubles, not 2 h: it is exact when they are within a factor of two (Sterbenz), and h
// itself may not be representable as a step from x. Theta is -dV/dT, as for the tape.
//
// Each pricing also returns the mean of |V| over its paths. It is free beside the path, and it is
// the scale of the round-off in the difference (bench/bump_study.hpp).
#ifndef AADP_PRICER_BUMP_HPP_
#define AADP_PRICER_BUMP_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "pricer/contract.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::pricer {

// Market order, which is the order of the tape's inputs and Greeks (src/ad/gradient.hpp).
enum class Input : std::uint8_t { kSpot, kSigma, kRate, kDividend, kMaturity };
inline constexpr std::size_t kInputCount = 5;

enum class BumpScheme : std::uint8_t { kCentral, kForward };

// One step size per input, in that input's own units.
using BumpSteps = std::array<double, kInputCount>;

// The market with one input moved by h.
Market<double> Bumped(const Market<double>& market, Input input, double h);

// One pricing, and the mean of |V| over its paths.
struct Revaluation {
  Estimate value;
  double mean_absolute = 0.0;
};

struct BumpEstimate {
  Estimate value;  // At the unbumped market: Price()'s, bit for bit.
  double mean_absolute = 0.0;
  std::array<Estimate, kInputCount> greeks;
  // Per input: the realised denominator, and mean |V| at the up and down markets (down is the
  // unbumped market for forward differences).
  std::array<double, kInputCount> denominator{};
  std::array<double, kInputCount> mean_absolute_up{};
  std::array<double, kInputCount> mean_absolute_down{};
  std::uint32_t pricings = 0;
};

struct BumpError {
  PriceError price;
  bool bumped = false;  // The refusal was of a bumped market, not the base one.
  Input input = Input::kSpot;
};

// Precondition: every step is positive and finite. A bumped market Validate refuses (a spot
// moved across its barrier, a maturity below the minimum) is refused with the input named.
std::expected<BumpEstimate, BumpError> PriceWithBumps(const Contract& contract,
                                                      const Market<double>& market,
                                                      const RunConfig& config, BumpScheme scheme,
                                                      const BumpSteps& steps);

// One revaluation, as PriceWithBumps makes it. Preconditions as for Price.
std::expected<Revaluation, PriceError> Revalue(const Contract& contract,
                                               const Market<double>& market,
                                               const RunConfig& config);

}  // namespace aadp::pricer

#endif  // AADP_PRICER_BUMP_HPP_
