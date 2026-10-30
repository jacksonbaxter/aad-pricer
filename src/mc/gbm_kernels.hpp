// Minimal path kernels for the oracles: GBM with exact log-normal steps, one uniform per step.
// A stub, not the pricer. Phase 3 replaces it and keeps the oracles.
//
// Each kernel is templated on Real and returns one discounted payoff for one path. S0, sigma,
// r, q and T are Real. Monitoring and fixing dates are t_i = i T / m, i = 1..m, so they scale
// with T, as theta = -dV/dT requires. Every kernel draws exactly one uniform per step from the
// caller, so a bumped run reuses the same numbers.
#ifndef AADP_MC_GBM_KERNELS_HPP_
#define AADP_MC_GBM_KERNELS_HPP_

#include <cmath>
#include <cstdint>
#include <span>

#include "oracle/black_scholes.hpp"
#include "rng/normal.hpp"

namespace aadp {

enum class Product {
  kEuropean,
  kAsianArithmetic,
  kAsianGeometric,
  kDownOutHard,     // Discrete, hard indicator. Its pathwise derivative is the naive one.
  kDownOutOneStep,  // Discrete, one-step survival.
  kDownOutBridge,   // Continuous, Brownian-bridge survival weight.
};

struct Contract {
  Product product = Product::kEuropean;
  OptionType type = OptionType::kCall;
  double strike = 0.0;
  double barrier = 0.0;       // Down-and-out kernels only.
  std::uint32_t steps = 1;    // Monitoring or fixing dates, the last at T.
  bool include_spot = false;  // Asians: S0 is one more point in the average.
};

namespace gbm_detail {

template <class Real>
struct Step {
  Real drift;
  Real sd;
  Real discount;
};

template <class Real>
Step<Real> MakeStep(const MarketParams<Real>& m, std::uint32_t steps) {
  using std::exp;
  using std::sqrt;
  const Real dt = m.maturity / static_cast<double>(steps);
  return {(m.r - m.q - 0.5 * m.sigma * m.sigma) * dt, m.sigma * sqrt(dt), exp(-m.r * m.maturity)};
}

template <class Real>
Real Payoff(OptionType type, const Real& underlying, double strike) {
  const Real intrinsic = type == OptionType::kCall ? underlying - strike : strike - underlying;
  return intrinsic > 0.0 ? intrinsic : Real(0.0);
}

template <class Real>
Real European(const Contract& c, const MarketParams<Real>& m, std::span<const double> uniforms) {
  using std::exp;
  using std::log;
  const Step<Real> step = MakeStep(m, c.steps);
  Real x = log(m.spot);
  for (std::uint32_t i = 0; i < c.steps; ++i)
    x = x + step.drift + step.sd * inverse_normal(uniforms[i]);
  return step.discount * Payoff(c.type, Real(exp(x)), c.strike);
}

template <class Real>
Real AsianArithmetic(const Contract& c, const MarketParams<Real>& m,
                     std::span<const double> uniforms) {
  using std::exp;
  using std::log;
  const Step<Real> step = MakeStep(m, c.steps);
  Real x = log(m.spot);
  Real sum = c.include_spot ? m.spot : Real(0.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    x = x + step.drift + step.sd * inverse_normal(uniforms[i]);
    sum = sum + exp(x);
  }
  const double count = static_cast<double>(c.steps) + (c.include_spot ? 1.0 : 0.0);
  return step.discount * Payoff(c.type, Real(sum / count), c.strike);
}

template <class Real>
Real AsianGeometric(const Contract& c, const MarketParams<Real>& m,
                    std::span<const double> uniforms) {
  using std::exp;
  using std::log;
  const Step<Real> step = MakeStep(m, c.steps);
  Real x = log(m.spot);
  Real sum = c.include_spot ? x : Real(0.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    x = x + step.drift + step.sd * inverse_normal(uniforms[i]);
    sum = sum + x;
  }
  const double count = static_cast<double>(c.steps) + (c.include_spot ? 1.0 : 0.0);
  return step.discount * Payoff(c.type, Real(exp(sum / count)), c.strike);
}

// The branch carries no derivative, which is what makes the pathwise delta wrong.
template <class Real>
Real DownOutHard(const Contract& c, const MarketParams<Real>& m, std::span<const double> uniforms) {
  using std::exp;
  using std::log;
  const Step<Real> step = MakeStep(m, c.steps);
  const double log_barrier = std::log(c.barrier);
  Real x = log(m.spot);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    x = x + step.drift + step.sd * inverse_normal(uniforms[i]);
    if (!(x > log_barrier)) return Real(0.0);
  }
  return step.discount * Payoff(c.type, Real(exp(x)), c.strike);
}

// Gerstner, Harrach, Roth, arXiv 1804.03975v3, Theorems 2.4 to 2.7. Each step has survival
// probability p = Phi(a), a = (x + drift - log H) / sd. The path is weighted by p and the step
// is drawn conditional on survival. With u uniform, z = -Phi^-1(p (1 - u)) has that conditional
// law: the same law as Phi^-1(1 - p + p u) on the same u. This form cannot round to Phi^-1(1) =
// inf, because p (1 - u) < 1 and u < 1 keeps it above zero whenever p is.
template <class Real>
Real DownOutOneStep(const Contract& c, const MarketParams<Real>& m,
                    std::span<const double> uniforms) {
  using std::exp;
  using std::log;
  const Step<Real> step = MakeStep(m, c.steps);
  const double log_barrier = std::log(c.barrier);
  Real x = log(m.spot);
  Real survival(1.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    const Real mean = x + step.drift;
    const Real p = normal_cdf((mean - log_barrier) / step.sd);
    if (!(p > 0.0)) return Real(0.0);
    survival = survival * p;
    const Real z = -inverse_normal(p * (1.0 - uniforms[i]));
    x = mean + step.sd * z;
  }
  return step.discount * survival * Payoff(c.type, Real(exp(x)), c.strike);
}

// Exact steps, then the probability that the bridge between two points above the barrier stayed
// above it: 1 - exp(-2 (x0 - b)(x1 - b) / (sigma^2 dt)). The weight falls to zero at the barrier,
// so the kill below is continuous and the estimator has no jump to lose.
template <class Real>
Real DownOutBridge(const Contract& c, const MarketParams<Real>& m,
                   std::span<const double> uniforms) {
  using std::exp;
  using std::log;
  const Step<Real> step = MakeStep(m, c.steps);
  const double log_barrier = std::log(c.barrier);
  const Real variance = step.sd * step.sd;
  Real x = log(m.spot);
  Real survival(1.0);
  for (std::uint32_t i = 0; i < c.steps; ++i) {
    const Real next = x + step.drift + step.sd * inverse_normal(uniforms[i]);
    if (!(next > log_barrier)) return Real(0.0);
    survival = survival * (1.0 - exp(-2.0 * (x - log_barrier) * (next - log_barrier) / variance));
    x = next;
  }
  return step.discount * survival * Payoff(c.type, Real(exp(x)), c.strike);
}

}  // namespace gbm_detail

template <class Real>
Real DiscountedPayoff(const Contract& c, const MarketParams<Real>& m,
                      std::span<const double> uniforms) {
  switch (c.product) {
    case Product::kEuropean:
      return gbm_detail::European(c, m, uniforms);
    case Product::kAsianArithmetic:
      return gbm_detail::AsianArithmetic(c, m, uniforms);
    case Product::kAsianGeometric:
      return gbm_detail::AsianGeometric(c, m, uniforms);
    case Product::kDownOutHard:
      return gbm_detail::DownOutHard(c, m, uniforms);
    case Product::kDownOutOneStep:
      return gbm_detail::DownOutOneStep(c, m, uniforms);
    case Product::kDownOutBridge:
      return gbm_detail::DownOutBridge(c, m, uniforms);
  }
  return Real(0.0);
}

}  // namespace aadp

#endif  // AADP_MC_GBM_KERNELS_HPP_
