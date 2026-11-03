// Path models. A kernel sees a model only through this interface:
//
//   Real Spot() const                  S0
//   Real LogSpot() const               log S0
//   Real Discount() const              exp(-r T)
//   StepLaw<Real> Law(i, x) const      the law of log S at date i + 1 given log S = x at date i:
//                                      normal with this mean and standard deviation
//
// Every treatment needs only that law. One-step survival and the last-step digital condition on
// it, the bridge reads its variance, and exact steps draw from it. So a log-Euler local-volatility
// model (stage 7) plugs in by evaluating sigma_loc(t_i, exp x) inside Law, with no kernel change.
// Under Euler the law is the scheme's, not the diffusion's, so the bridge then carries a bias of
// first order in the step (Gobet 2000), which is measured there, not assumed away.
#ifndef AADP_PRICER_MODEL_HPP_
#define AADP_PRICER_MODEL_HPP_

#include <cstdint>

#include "pricer/contract.hpp"
#include "pricer/real_ops.hpp"

namespace aadp::pricer {

template <class Real>
struct StepLaw {
  Real mean;
  Real sd;
};

// GBM, exact log-space steps: x_{i+1} = x_i + (r - q - sigma^2 / 2) dt + sigma sqrt(dt) z. The
// operations and their order are those of src/mc/gbm_kernels.hpp, MakeStep, so the two differ
// only by the exp, log and inverse normal they call.
template <class Real>
class GbmModel {
 public:
  GbmModel(const Market<Real>& m, std::uint32_t steps)
      : spot_(m.spot),
        log_spot_(Log(m.spot)),
        discount_(Exp(-m.r * m.maturity)),
        drift_((m.r - m.q - 0.5 * m.sigma * m.sigma) * (m.maturity / static_cast<double>(steps))),
        sd_(m.sigma * Sqrt(m.maturity / static_cast<double>(steps))) {}

  [[nodiscard]] const Real& Spot() const { return spot_; }
  [[nodiscard]] const Real& LogSpot() const { return log_spot_; }
  [[nodiscard]] const Real& Discount() const { return discount_; }
  [[nodiscard]] StepLaw<Real> Law(std::uint32_t /*step*/, const Real& x) const {
    return {x + drift_, sd_};
  }

 private:
  Real spot_;
  Real log_spot_;
  Real discount_;
  Real drift_;
  Real sd_;
};

}  // namespace aadp::pricer

#endif  // AADP_PRICER_MODEL_HPP_
