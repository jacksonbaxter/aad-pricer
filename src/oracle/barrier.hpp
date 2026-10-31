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

enum class BarrierType : std::uint8_t { kDownIn, kUpIn, kDownOut, kUpOut };

namespace barrier_detail {

using std::exp;
using std::log;
using std::pow;
using std::sqrt;

// Haug's A to F terms. The expressions are unchanged from the formula sheet, phi and eta are
// its signs.
template <class Real>
struct Terms {
  Terms(double strike_in, double barrier_in, double rebate_in, const MarketParams<Real>& market)
      : strike(strike_in),
        barrier(barrier_in),
        rebate(rebate_in),
        m(market),
        sd(m.sigma * sqrt(m.maturity)),
        df_q(exp(-m.q * m.maturity)),
        df_r(exp(-m.r * m.maturity)),
        variance_rate(m.sigma * m.sigma),
        mu((m.r - m.q) / variance_rate - 0.5),
        drift_sd((1.0 + mu) * sd),
        barrier_ratio(barrier / m.spot),
        reflect_strike(pow(barrier_ratio, 2.0 * mu)),
        reflect_spot(reflect_strike * barrier_ratio * barrier_ratio),
        x1(log(m.spot / strike) / sd + drift_sd),
        x2(log(m.spot / barrier) / sd + drift_sd),
        y1(log(barrier * barrier_ratio / strike) / sd + drift_sd),
        y2(log(barrier / m.spot) / sd + drift_sd) {}

  [[nodiscard]] Real Vanilla(const Real& x, double phi) const {
    return phi * (m.spot * df_q * normal_cdf(phi * x) - strike * df_r * normal_cdf(phi * (x - sd)));
  }
  [[nodiscard]] Real Reflected(const Real& y, double eta, double phi) const {
    return phi * (m.spot * df_q * reflect_spot * normal_cdf(eta * y) -
                  strike * df_r * reflect_strike * normal_cdf(eta * (y - sd)));
  }
  [[nodiscard]] Real A(double phi) const { return Vanilla(x1, phi); }
  [[nodiscard]] Real B(double phi) const { return Vanilla(x2, phi); }
  [[nodiscard]] Real C(double eta, double phi) const { return Reflected(y1, eta, phi); }
  [[nodiscard]] Real D(double eta, double phi) const { return Reflected(y2, eta, phi); }
  [[nodiscard]] Real E(double eta) const {
    if (rebate == 0.0) return Real(0.0);
    return rebate * df_r *
           (normal_cdf(eta * (x2 - sd)) - reflect_strike * normal_cdf(eta * (y2 - sd)));
  }
  [[nodiscard]] Real F(double eta) const {
    if (rebate == 0.0) return Real(0.0);
    const Real lambda = sqrt(mu * mu + 2.0 * m.r / variance_rate);
    const Real z = log(barrier / m.spot) / sd + lambda * sd;
    return rebate * (pow(barrier_ratio, mu + lambda) * normal_cdf(eta * z) +
                     pow(barrier_ratio, mu - lambda) * normal_cdf(eta * (z - 2.0 * lambda * sd)));
  }

  double strike;
  double barrier;
  double rebate;
  MarketParams<Real> m;
  Real sd;
  Real df_q;
  Real df_r;
  Real variance_rate;
  Real mu;
  Real drift_sd;
  Real barrier_ratio;
  Real reflect_strike;
  Real reflect_spot;
  Real x1;
  Real x2;
  Real y1;
  Real y2;
};

template <class Real>
Real CallValue(const Terms<Real>& t, BarrierType barrier_type) {
  const bool strike_above = t.strike >= t.barrier;
  switch (barrier_type) {
    case BarrierType::kDownIn:
      return (strike_above ? t.C(1, 1) : t.A(1) - t.B(1) + t.D(1, 1)) + t.E(1);
    case BarrierType::kUpIn:
      return (strike_above ? t.A(1) : t.B(1) - t.C(-1, 1) + t.D(-1, 1)) + t.E(-1);
    case BarrierType::kDownOut:
      return (strike_above ? t.A(1) - t.C(1, 1) : t.B(1) - t.D(1, 1)) + t.F(1);
    case BarrierType::kUpOut:
      return (strike_above ? Real(0.0) : t.A(1) - t.B(1) + t.C(-1, 1) - t.D(-1, 1)) + t.F(-1);
  }
  return Real(0.0);
}

template <class Real>
Real PutValue(const Terms<Real>& t, BarrierType barrier_type) {
  const bool strike_above = t.strike >= t.barrier;
  switch (barrier_type) {
    case BarrierType::kDownIn:
      return (strike_above ? t.B(-1) - t.C(1, -1) + t.D(1, -1) : t.A(-1)) + t.E(1);
    case BarrierType::kUpIn:
      return (strike_above ? t.A(-1) - t.B(-1) + t.D(-1, -1) : t.C(-1, -1)) + t.E(-1);
    case BarrierType::kDownOut:
      return (strike_above ? t.A(-1) - t.B(-1) + t.C(1, -1) - t.D(1, -1) : Real(0.0)) + t.F(1);
    case BarrierType::kUpOut:
      return (strike_above ? t.B(-1) - t.D(-1, -1) : t.A(-1) - t.C(-1, -1)) + t.F(-1);
  }
  return Real(0.0);
}

}  // namespace barrier_detail

template <class Real>
Real BarrierOptionValue(BarrierType barrier_type, OptionType option_type, double strike,
                        double barrier, double rebate, const MarketParams<Real>& m) {
  const barrier_detail::Terms<Real> terms(strike, barrier, rebate, m);
  return option_type == OptionType::kCall ? barrier_detail::CallValue(terms, barrier_type)
                                          : barrier_detail::PutValue(terms, barrier_type);
}

}  // namespace aadp

#endif  // AADP_ORACLE_BARRIER_HPP_
