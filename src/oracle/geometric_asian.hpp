// Discrete geometric average-price Asian, closed form, templated on Real.
// log G is normal: mean log S + nu * mean(t), variance sigma^2 / M^2 * sum_ij min(t_i, t_j),
// with nu = r - q - sigma^2 / 2 (Kemna and Vorst 1990 for the continuous case; the discrete
// variance is the covariance of Brownian motion). Greek conventions: see black_scholes.hpp.
//
// Fixings sit at t_i = i T / n for i = 1..n, plus t_0 = 0 if include_spot. They scale with T.
#ifndef AADP_ORACLE_GEOMETRIC_ASIAN_HPP_
#define AADP_ORACLE_GEOMETRIC_ASIAN_HPP_

#include <cmath>
#include <cstdint>

#include "oracle/black_scholes.hpp"
#include "rng/normal.hpp"

namespace aadp {

template <class Real>
Real GeometricAsianValue(OptionType type, double strike, std::uint32_t fixings, bool include_spot,
                         const MarketParams<Real>& m) {
  using std::exp;
  using std::log;
  using std::sqrt;
  const auto n = static_cast<double>(fixings);
  const double count = n + (include_spot ? 1.0 : 0.0);
  // Fractions of T. sum_i i/n, and sum_ij min(i, j)/n over i, j = 1..n, where a fixing at 0
  // adds nothing to either sum.
  const double mean_fraction = (n + 1.0) / 2.0 / count;
  const double variance_fraction = (n + 1.0) * (2.0 * n + 1.0) / 6.0 / (count * count);

  const Real nu = m.r - m.q - 0.5 * m.sigma * m.sigma;
  const Real mean = log(m.spot) + nu * m.maturity * mean_fraction;
  const Real variance = m.sigma * m.sigma * m.maturity * variance_fraction;
  const Real sd = sqrt(variance);
  const Real forward = exp(mean + 0.5 * variance);
  const Real d1 = (mean - log(strike) + variance) / sd;
  const Real d2 = d1 - sd;
  const double phi = type == OptionType::kCall ? 1.0 : -1.0;
  return phi * exp(-m.r * m.maturity) *
         (forward * normal_cdf(phi * d1) - strike * normal_cdf(phi * d2));
}

}  // namespace aadp

#endif  // AADP_ORACLE_GEOMETRIC_ASIAN_HPP_
