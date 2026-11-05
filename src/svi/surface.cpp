#include "svi/surface.hpp"

#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "math/exp.hpp"
#include "math/log.hpp"
#include "svi/raw_svi.hpp"

namespace aadp::svi {

namespace {

// False on NaN.
bool InRange(double x, double lo, double hi) { return x >= lo && x <= hi; }

bool ValidParameters(const RawSvi<double>& p) {
  return InRange(p.a, -kMaxSurfaceParameter, kMaxSurfaceParameter) &&
         InRange(p.b, 0.0, kMaxSurfaceParameter) && InRange(p.rho, -1.0, 1.0) &&
         InRange(p.m, -kMaxSurfaceParameter, kMaxSurfaceParameter) &&
         InRange(p.sigma, kMinSurfaceSigma, kMaxSurfaceParameter);
}

}  // namespace

const char* SurfaceErrorName(SurfaceError error) noexcept {
  switch (error) {
    case SurfaceError::kNone:
      return "none";
    case SurfaceError::kSliceCount:
      return "no slices, too many, or terms and parameters of different sizes";
    case SurfaceError::kExpiry:
      return "expiry not finite, out of range or not increasing by at least the minimum gap";
    case SurfaceError::kForward:
      return "forward not finite or out of range";
    case SurfaceError::kDiscount:
      return "discount factor not finite or out of range";
    case SurfaceError::kParameters:
      return "slice parameters not finite, out of range, b < 0, |rho| > 1 or sigma too small";
  }
  return "unknown error";
}

SurfaceError ValidateSurface(std::span<const SliceTerms> terms,
                             std::span<const RawSvi<double>> params) {
  if (terms.empty() || terms.size() > kMaxSurfaceSlices || terms.size() != params.size()) {
    return SurfaceError::kSliceCount;
  }
  double previous = 0.0;
  for (std::size_t j = 0; j < terms.size(); ++j) {
    const SliceTerms& t = terms[j];
    if (!InRange(t.expiry, kMinSurfaceTime, kMaxSurfaceExpiry) ||
        !(t.expiry - previous >= kMinSurfaceTime)) {
      return SurfaceError::kExpiry;
    }
    previous = t.expiry;
    if (!InRange(t.forward, kMinCurveLevel, kMaxCurveLevel)) return SurfaceError::kForward;
    if (!InRange(t.discount, kMinCurveLevel, kMaxCurveLevel)) return SurfaceError::kDiscount;
    if (!ValidParameters(params[j])) return SurfaceError::kParameters;
  }
  return SurfaceError::kNone;
}

ForwardCurve::ForwardCurve(std::span<const SliceTerms> terms)
    : spot_(terms.front().discount * terms.front().forward) {
  times_.reserve(terms.size() + 1);
  log_forward_.reserve(terms.size() + 1);
  log_discount_.reserve(terms.size() + 1);
  times_.push_back(0.0);
  log_forward_.push_back(math::log(spot_));
  log_discount_.push_back(0.0);
  for (const SliceTerms& t : terms) {
    times_.push_back(t.expiry);
    log_forward_.push_back(math::log(t.forward));
    log_discount_.push_back(math::log(t.discount));
  }
}

double ForwardCurve::Interpolate(const std::vector<double>& values, double t) const {
  if (!(t > 0.0)) return values.front();
  if (t >= times_.back()) return values.back();
  std::size_t i = 1;
  while (times_[i] < t) ++i;
  // At a node its own value, exactly: a + (b - a) need not round to b.
  if (times_[i] == t) return values[i];
  const double weight = (t - times_[i - 1]) / (times_[i] - times_[i - 1]);
  return values[i - 1] + (weight * (values[i] - values[i - 1]));
}

double ForwardCurve::Discount(double t) const { return math::exp(LogDiscount(t)); }

TimeSlot Locate(std::span<const double> expiries, double t) {
  const std::size_t n = expiries.size();
  if (n == 1 || t < expiries[0]) {
    return {.segment = 0, .weight = t / expiries[0], .inverse_length = 1.0 / expiries[0]};
  }
  std::size_t s = 1;
  while (s + 1 < n && expiries[s] <= t) ++s;
  const double length = expiries[s] - expiries[s - 1];
  return {.segment = s, .weight = (t - expiries[s - 1]) / length, .inverse_length = 1.0 / length};
}

}  // namespace aadp::svi
