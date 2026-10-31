// Closed-form Black-Scholes-Merton value and Greeks, templated on Real.
//
// Conventions for every oracle in this directory:
// - Greeks are per unit: delta per 1 of spot, vega per 1.00 of volatility, rho and dividend rho
//   per 1.00 of rate. Nothing is scaled to 1% or to one basis point.
// - Theta is per year, and theta = -dV/dT, the sensitivity to shortening maturity at fixed
//   spot. With time-proportional fixing dates, those dates scale with T.
// - Rates are continuously compounded. Volatility is annualised.
#ifndef AADP_ORACLE_BLACK_SCHOLES_HPP_
#define AADP_ORACLE_BLACK_SCHOLES_HPP_

#include <cmath>
#include <cstdint>

#include "rng/normal.hpp"

namespace aadp {

enum class OptionType : std::uint8_t { kCall, kPut };

template <class Real>
struct MarketParams {
  Real spot;
  Real sigma;
  Real r;
  Real q;
  Real maturity;
};

template <class Real>
struct BsGreeks {
  Real value;
  Real delta;
  Real gamma;
  Real vega;
  Real rho;
  Real dividend_rho;
  Real theta;
};

template <class Real>
BsGreeks<Real> BlackScholes(OptionType type, double strike, const MarketParams<Real>& m) {
  using std::exp;
  using std::log;
  using std::sqrt;
  const double phi = type == OptionType::kCall ? 1.0 : -1.0;
  const Real root_t = sqrt(m.maturity);
  const Real sd = m.sigma * root_t;
  const Real d1 = (log(m.spot / strike) + (m.r - m.q + 0.5 * m.sigma * m.sigma) * m.maturity) / sd;
  const Real d2 = d1 - sd;
  const Real df_r = exp(-m.r * m.maturity);
  const Real df_q = exp(-m.q * m.maturity);
  const Real cdf1 = normal_cdf(phi * d1);
  const Real cdf2 = normal_cdf(phi * d2);
  const Real pdf1 = normal_pdf(d1);
  BsGreeks<Real> g{};
  g.value = phi * (m.spot * df_q * cdf1 - strike * df_r * cdf2);
  g.delta = phi * df_q * cdf1;
  g.gamma = df_q * pdf1 / (m.spot * sd);
  g.vega = m.spot * df_q * pdf1 * root_t;
  g.rho = phi * strike * m.maturity * df_r * cdf2;
  g.dividend_rho = -phi * m.spot * m.maturity * df_q * cdf1;
  g.theta = -m.spot * df_q * pdf1 * m.sigma / (2.0 * root_t) +
            phi * (m.q * m.spot * df_q * cdf1 - m.r * strike * df_r * cdf2);
  return g;
}

}  // namespace aadp

#endif  // AADP_ORACLE_BLACK_SCHOLES_HPP_
