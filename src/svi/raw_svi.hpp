// Raw SVI total implied variance, its first two derivatives in log-moneyness, the Durrleman
// function g and d+-, templated on Real so that stage 7 can record them on the tape.
//
// Gatheral and Jacquier, "Arbitrage-free SVI volatility surfaces", arXiv 1204.0646:
//   w(k)  = a + b (rho (k - m) + sqrt((k - m)^2 + sigma^2))                       eq. (3.1)
//   g(k)  = (1 - k w'/(2 w))^2 - (w'^2 / 4) (1/w + 1/4) + w''/2                    eq. (2.1)
//   d+-(k) = -k / sqrt(w) +- sqrt(w) / 2                                           section 2.2
// with k = ln(K / F). The derivatives follow from (3.1), as in Martini and Mingone, arXiv
// 2005.03340, Lemma 4.1:
//   w'(k)  = b (rho + (k - m) / sqrt((k - m)^2 + sigma^2))
//   w''(k) = b sigma^2 / ((k - m)^2 + sigma^2)^(3/2)
// GJ Lemma 2.2: a slice is free of butterfly arbitrage if and only if g(k) >= 0 for all k and
// d+(k) -> -infinity as k -> +infinity. The checks are in arbitrage.hpp.
//
// A Real needs + - * / with Real and double, construction from double and an unqualified Sqrt,
// which argument-dependent lookup finds for the tape's type (src/ad/real.hpp) and the Dual; the
// double one below is src/math/sqrt, IEEE sqrt. Nothing here branches on a Real.
#ifndef AADP_SVI_RAW_SVI_HPP_
#define AADP_SVI_RAW_SVI_HPP_

#include "math/sqrt.hpp"

namespace aadp::svi {

inline double Sqrt(double x) { return math::sqrt(x); }

template <class Real>
struct RawSvi {
  Real a{};
  Real b{};
  Real rho{};
  Real m{};
  Real sigma{};
};

// w, w' and w'' at one k, sharing the square root.
template <class Real>
struct SviPoint {
  Real w;
  Real w1;
  Real w2;
};

template <class Real>
SviPoint<Real> Evaluate(const RawSvi<Real>& p, const Real& k) {
  const Real x = k - p.m;
  const Real r = Sqrt(x * x + p.sigma * p.sigma);
  return {.w = p.a + p.b * (p.rho * x + r),
          .w1 = p.b * (p.rho + x / r),
          .w2 = p.b * p.sigma * p.sigma / (r * r * r)};
}

template <class Real>
Real TotalVariance(const RawSvi<Real>& p, const Real& k) {
  const Real x = k - p.m;
  return p.a + p.b * (p.rho * x + Sqrt(x * x + p.sigma * p.sigma));
}

// GJ eq. (2.1), from w, w' and w'' at k. Defined where w > 0.
template <class Real>
Real Durrleman(const SviPoint<Real>& s, const Real& k) {
  const Real u = 1.0 - k * s.w1 / (2.0 * s.w);
  return u * u - 0.25 * s.w1 * s.w1 * (1.0 / s.w + 0.25) + 0.5 * s.w2;
}

template <class Real>
Real Durrleman(const RawSvi<Real>& p, const Real& k) {
  return Durrleman(Evaluate(p, k), k);
}

// GJ section 2.2, from the total variance w at k. Defined where w > 0.
template <class Real>
Real DPlus(const Real& w, const Real& k) {
  const Real root = Sqrt(w);
  return -k / root + 0.5 * root;
}

template <class Real>
Real DMinus(const Real& w, const Real& k) {
  const Real root = Sqrt(w);
  return -k / root - 0.5 * root;
}

}  // namespace aadp::svi

#endif  // AADP_SVI_RAW_SVI_HPP_
