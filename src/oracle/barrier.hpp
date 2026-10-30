// Continuously monitored single barrier options, closed form, all eight cases.
// Reiner and Rubinstein 1991, in the A to F decomposition as printed in Haug, "Option Pricing
// Formulas", 1998, pp. 69-72. Greek conventions: see black_scholes.hpp.
//
// A rebate on a knock-out is paid at the hit. On a knock-in it is paid at expiry if the barrier
// was never hit. Precondition: the spot has not yet crossed the barrier.
#ifndef AADP_ORACLE_BARRIER_HPP_
#define AADP_ORACLE_BARRIER_HPP_

#include <cmath>

#include "oracle/black_scholes.hpp"
#include "rng/normal.hpp"

namespace aadp {

enum class BarrierType { kDownIn, kUpIn, kDownOut, kUpOut };

template <class Real>
Real BarrierOptionValue(BarrierType barrier_type, OptionType option_type, double strike,
                        double barrier, double rebate, const MarketParams<Real>& m) {
  using std::exp;
  using std::log;
  using std::pow;
  using std::sqrt;
  const Real sd = m.sigma * sqrt(m.maturity);
  const Real df_q = exp(-m.q * m.maturity);
  const Real df_r = exp(-m.r * m.maturity);
  const Real variance_rate = m.sigma * m.sigma;
  const Real mu = (m.r - m.q) / variance_rate - 0.5;
  const Real drift_sd = (1.0 + mu) * sd;
  const Real barrier_ratio = barrier / m.spot;
  const Real reflect_strike = pow(barrier_ratio, 2.0 * mu);
  const Real reflect_spot = reflect_strike * barrier_ratio * barrier_ratio;

  const Real x1 = log(m.spot / strike) / sd + drift_sd;
  const Real x2 = log(m.spot / barrier) / sd + drift_sd;
  const Real y1 = log(barrier * barrier_ratio / strike) / sd + drift_sd;
  const Real y2 = log(barrier / m.spot) / sd + drift_sd;

  const auto vanilla_term = [&](const Real& x, double phi) {
    return phi * (m.spot * df_q * normal_cdf(phi * x) - strike * df_r * normal_cdf(phi * (x - sd)));
  };
  const auto a_term = [&](double phi) { return vanilla_term(x1, phi); };
  const auto b_term = [&](double phi) { return vanilla_term(x2, phi); };
  const auto reflected_term = [&](const Real& y, double eta, double phi) {
    return phi * (m.spot * df_q * reflect_spot * normal_cdf(eta * y) -
                  strike * df_r * reflect_strike * normal_cdf(eta * (y - sd)));
  };
  const auto c_term = [&](double eta, double phi) { return reflected_term(y1, eta, phi); };
  const auto d_term = [&](double eta, double phi) { return reflected_term(y2, eta, phi); };
  const auto e_term = [&](double eta) -> Real {
    if (rebate == 0.0) return Real(0.0);
    return rebate * df_r *
           (normal_cdf(eta * (x2 - sd)) - reflect_strike * normal_cdf(eta * (y2 - sd)));
  };
  const auto f_term = [&](double eta) -> Real {
    if (rebate == 0.0) return Real(0.0);
    const Real lambda = sqrt(mu * mu + 2.0 * m.r / variance_rate);
    const Real z = log(barrier / m.spot) / sd + lambda * sd;
    return rebate * (pow(barrier_ratio, mu + lambda) * normal_cdf(eta * z) +
                     pow(barrier_ratio, mu - lambda) * normal_cdf(eta * (z - 2.0 * lambda * sd)));
  };

  const bool strike_above = strike >= barrier;
  if (option_type == OptionType::kCall) {
    switch (barrier_type) {
      case BarrierType::kDownIn:
        return (strike_above ? c_term(1, 1) : a_term(1) - b_term(1) + d_term(1, 1)) + e_term(1);
      case BarrierType::kUpIn:
        return (strike_above ? a_term(1) : b_term(1) - c_term(-1, 1) + d_term(-1, 1)) + e_term(-1);
      case BarrierType::kDownOut:
        return (strike_above ? a_term(1) - c_term(1, 1) : b_term(1) - d_term(1, 1)) + f_term(1);
      case BarrierType::kUpOut:
        return (strike_above ? Real(0.0) : a_term(1) - b_term(1) + c_term(-1, 1) - d_term(-1, 1)) +
               f_term(-1);
    }
  }
  switch (barrier_type) {
    case BarrierType::kDownIn:
      return (strike_above ? b_term(-1) - c_term(1, -1) + d_term(1, -1) : a_term(-1)) + e_term(1);
    case BarrierType::kUpIn:
      return (strike_above ? a_term(-1) - b_term(-1) + d_term(-1, -1) : c_term(-1, -1)) +
             e_term(-1);
    case BarrierType::kDownOut:
      return (strike_above ? a_term(-1) - b_term(-1) + c_term(1, -1) - d_term(1, -1) : Real(0.0)) +
             f_term(1);
    case BarrierType::kUpOut:
      return (strike_above ? b_term(-1) - d_term(-1, -1) : a_term(-1) - c_term(-1, -1)) +
             f_term(-1);
  }
  return Real(0.0);
}

}  // namespace aadp

#endif  // AADP_ORACLE_BARRIER_HPP_
