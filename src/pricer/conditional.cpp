#include <cmath>

#include "math/sqrt.hpp"
#include "pricer/conditional_kernels.hpp"
#include "pricer/contract.hpp"

namespace aadp::pricer {

ConditionalError ValidateConditional(const Contract& c, const Market<double>& m) {
  if (c.product != Product::kBarrier) return ConditionalError::kNone;
  if (c.barrier.treatment == Treatment::kHard) return ConditionalError::kIndicator;
  if (c.barrier.treatment == Treatment::kSmoothed) return ConditionalError::kSmoothed;
  if (c.barrier.monitoring == Monitoring::kContinuous) {
    // Keeps the cap on the reflection exponent harmless (conditional_kernels.hpp). False on NaN.
    const double dt = m.maturity / static_cast<double>(c.steps);
    const double drift = std::fabs(m.r - m.q - (0.5 * m.sigma * m.sigma)) * dt;
    if (!(drift <= kMaxConditionalDriftSd * m.sigma * math::sqrt(dt))) {
      return ConditionalError::kDriftPerStep;
    }
  }
  return ConditionalError::kNone;
}

const char* ErrorName(ConditionalError error) {
  switch (error) {
    case ConditionalError::kNone:
      return "none";
    case ConditionalError::kIndicator:
      return "an indicator's pathwise second derivatives are zero: use kOneStep or kBridge";
    case ConditionalError::kSmoothed:
      return "the smoothed barrier is a biased estimator by design: use kOneStep";
    case ConditionalError::kDriftPerStep:
      return "drift per step above 4 standard deviations on a continuous barrier";
  }
  return "unknown error";
}

}  // namespace aadp::pricer
