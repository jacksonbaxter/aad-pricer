// Central-difference oracle, per path, with common random numbers. The bumped runs reuse the
// path's uniforms and call the same kernel as the estimator under test, so they share its
// treatment. Conventions as in black_scholes.hpp: theta = -dV/dT.
#ifndef AADP_ORACLE_FINITE_DIFFERENCE_HPP_
#define AADP_ORACLE_FINITE_DIFFERENCE_HPP_

#include <span>

#include "mc/gbm_kernels.hpp"
#include "oracle/black_scholes.hpp"

namespace aadp {

enum class Greek : std::uint8_t { kValue, kDelta, kVega, kRho, kDividendRho, kTheta };

inline constexpr int kFirstOrderGreeks = 5;

inline MarketParams<double> BumpFor(Greek greek, MarketParams<double> m, double bump) {
  switch (greek) {
    case Greek::kDelta:
      m.spot += bump;
      break;
    case Greek::kVega:
      m.sigma += bump;
      break;
    case Greek::kRho:
      m.r += bump;
      break;
    case Greek::kDividendRho:
      m.q += bump;
      break;
    case Greek::kTheta:
      m.maturity += bump;
      break;
    case Greek::kValue:
      break;
  }
  return m;
}

// (V(+h) - V(-h)) / 2h on one path, with theta's sign applied.
inline double PathCentralDifference(const Contract& contract, const MarketParams<double>& m,
                                    Greek greek, double h, std::span<const double> uniforms) {
  const auto up = DiscountedPayoff<double>(contract, BumpFor(greek, m, h), uniforms);
  const auto down = DiscountedPayoff<double>(contract, BumpFor(greek, m, -h), uniforms);
  const double slope = (up - down) / (2.0 * h);
  return greek == Greek::kTheta ? -slope : slope;
}

}  // namespace aadp

#endif  // AADP_ORACLE_FINITE_DIFFERENCE_HPP_
