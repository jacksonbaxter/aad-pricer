#include "svi/local_vol.hpp"

#include <cmath>

namespace aadp::svi {

const char* LocalVarianceClassName(LocalVarianceClass c) noexcept {
  switch (c) {
    case LocalVarianceClass::kOk:
      return "ok";
    case LocalVarianceClass::kNonFinite:
      return "non-finite";
    case LocalVarianceClass::kNonPositiveVariance:
      return "non-positive-variance";
    case LocalVarianceClass::kCalendar:
      return "calendar";
    case LocalVarianceClass::kButterfly:
      return "butterfly";
    case LocalVarianceClass::kBelowFloor:
      return "below-floor";
    case LocalVarianceClass::kCapped:
      return "capped";
  }
  return "unknown";
}

LocalVarianceClass Classify(double w_pos, double dwdt, double g, const LocalVolRule& rule) {
  if (!std::isfinite(w_pos) || !std::isfinite(dwdt) || !std::isfinite(g)) {
    return LocalVarianceClass::kNonFinite;
  }
  if (!(w_pos > kAdmissibleThreshold)) return LocalVarianceClass::kNonPositiveVariance;
  if (!(dwdt > kAdmissibleThreshold)) return LocalVarianceClass::kCalendar;
  if (!(g > kAdmissibleThreshold)) return LocalVarianceClass::kButterfly;
  if (dwdt <= rule.floor * g) return LocalVarianceClass::kBelowFloor;
  if (dwdt >= rule.cap * g) return LocalVarianceClass::kCapped;
  return LocalVarianceClass::kOk;
}

}  // namespace aadp::svi
