// The contracts of references/comparison_plan.csv, as src/oracle/plan.hpp decodes them, in the
// pricer's terms. The plan's kinds map one to one:
//   european, arithmetic, geometric   the same product, same dates, same include_spot
//   hard                              down-and-out, discrete, hard indicator
//   one_step                          down-and-out, discrete, one-step survival
//   bridge                            down-and-out, continuous, bridge
// Separate from the pricer itself, so the pricer does not depend on the oracle's decoder.
#ifndef AADP_PRICER_FROM_PLAN_HPP_
#define AADP_PRICER_FROM_PLAN_HPP_

#include "mc/gbm_kernels.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/plan.hpp"
#include "pricer/contract.hpp"

namespace aadp::pricer {

inline Contract FromPlan(const aadp::Contract& in) {
  Contract out;
  out.type = in.type == aadp::OptionType::kCall ? OptionType::kCall : OptionType::kPut;
  out.strike = in.strike;
  out.steps = in.steps;
  out.include_spot = in.include_spot;
  const auto barrier = [&](Monitoring monitoring, Treatment treatment) {
    out.product = Product::kBarrier;
    out.barrier = {.direction = Direction::kDown,
                   .knock = Knock::kOut,
                   .monitoring = monitoring,
                   .treatment = treatment,
                   .level = in.barrier,
                   .smoothing_width = 0.0};
  };
  switch (in.product) {
    case aadp::Product::kEuropean:
      out.product = Product::kEuropean;
      break;
    case aadp::Product::kAsianArithmetic:
      out.product = Product::kAsianArithmetic;
      break;
    case aadp::Product::kAsianGeometric:
      out.product = Product::kAsianGeometric;
      break;
    case aadp::Product::kDownOutHard:
      barrier(Monitoring::kDiscrete, Treatment::kHard);
      break;
    case aadp::Product::kDownOutOneStep:
      barrier(Monitoring::kDiscrete, Treatment::kOneStep);
      break;
    case aadp::Product::kDownOutBridge:
      barrier(Monitoring::kContinuous, Treatment::kBridge);
      break;
  }
  return out;
}

template <class Real>
Market<Real> FromPlan(const aadp::MarketParams<Real>& m) {
  return {.spot = m.spot, .sigma = m.sigma, .r = m.r, .q = m.q, .maturity = m.maturity};
}

}  // namespace aadp::pricer

#endif  // AADP_PRICER_FROM_PLAN_HPP_
