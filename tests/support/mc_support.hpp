// Glue shared by the Monte Carlo tests and the oracle runner: seeds a market with one Dual
// tangent per Greek and reads the Greeks back.
#ifndef AADP_TESTS_SUPPORT_MC_SUPPORT_HPP_
#define AADP_TESTS_SUPPORT_MC_SUPPORT_HPP_

#include <cstddef>

#include "oracle/black_scholes.hpp"
#include "oracle/finite_difference.hpp"
#include "support/dual.hpp"

namespace aadp::test {

inline constexpr std::size_t kSlotSpot = 0;
inline constexpr std::size_t kSlotSigma = 1;
inline constexpr std::size_t kSlotR = 2;
inline constexpr std::size_t kSlotQ = 3;
inline constexpr std::size_t kSlotMaturity = 4;

inline MarketParams<Dual> ActiveMarket(const MarketParams<double>& m) {
  return {Dual::Variable(m.spot, kSlotSpot), Dual::Variable(m.sigma, kSlotSigma),
          Dual::Variable(m.r, kSlotR), Dual::Variable(m.q, kSlotQ),
          Dual::Variable(m.maturity, kSlotMaturity)};
}

// Value, or a first-order Greek with theta = -dV/dT.
inline double ReadGreek(const Dual& x, Greek greek) {
  switch (greek) {
    case Greek::kValue:
      return x.value;
    case Greek::kDelta:
      return x.tangent[kSlotSpot];
    case Greek::kVega:
      return x.tangent[kSlotSigma];
    case Greek::kRho:
      return x.tangent[kSlotR];
    case Greek::kDividendRho:
      return x.tangent[kSlotQ];
    case Greek::kTheta:
      return -x.tangent[kSlotMaturity];
  }
  return 0.0;
}

}  // namespace aadp::test

#endif  // AADP_TESTS_SUPPORT_MC_SUPPORT_HPP_
