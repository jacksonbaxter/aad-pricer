#include "svi/essvi.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace aadp::svi {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool Positive(double x) { return std::isfinite(x) && x > 0.0; }

bool ValidSlice(const EssviSlice<double>& s) {
  return Positive(s.theta) && Positive(s.psi) && std::isfinite(s.rho) && std::fabs(s.rho) < 1.0;
}

void Add(EssviConstraintReport& report, EssviConstraint constraint, std::size_t slice,
         double slack) {
  const bool strict = constraint == EssviConstraint::kLee;
  const bool violated =
      !(slack >= (strict ? kEssviConstraintTolerance : -kEssviConstraintTolerance));
  report.states.push_back({.constraint = constraint,
                           .slice = slice,
                           .slack = slack,
                           .active = std::fabs(slack) <= kActiveSlack,
                           .violated = violated});
  if (violated) ++report.violations;
}

}  // namespace

const char* PsiCapName(PsiCap cap) noexcept {
  switch (cap) {
    case PsiCap::kLee:
      return "B1 psi (1 + |rho|) < 4";
    case PsiCap::kGatheralJacquier:
      return "B2 psi^2 (1 + |rho|) <= 4 theta";
    case PsiCap::kThetaRatio:
      return "S psi_i / psi_{i-1} <= theta_i / theta_{i-1}";
    case PsiCap::kLaterSlice:
      return "room for a later slice";
  }
  return "unknown";
}

const char* EssviConstraintName(EssviConstraint constraint) noexcept {
  switch (constraint) {
    case EssviConstraint::kParameters:
      return "parameters";
    case EssviConstraint::kLee:
      return "B1";
    case EssviConstraint::kGatheralJacquier:
      return "B2";
    case EssviConstraint::kThetaOrder:
      return "N-theta";
    case EssviConstraint::kPsiLower:
      return "N-psi";
    case EssviConstraint::kThetaRatio:
      return "S";
  }
  return "unknown";
}

const char* EssviCoordinateName(EssviCoordinate coordinate) noexcept {
  switch (coordinate) {
    case EssviCoordinate::kRho:
      return "rho";
    case EssviCoordinate::kTheta1:
      return "theta_1";
    case EssviCoordinate::kA:
      return "a";
    case EssviCoordinate::kC:
      return "c";
  }
  return "unknown";
}

EssviConstraintReport CheckEssviConstraints(std::span<const EssviSlice<double>> slices) {
  EssviConstraintReport report;
  for (std::size_t i = 0; i < slices.size(); ++i) {
    const EssviSlice<double>& s = slices[i];
    if (!ValidSlice(s)) {
      Add(report, EssviConstraint::kParameters, i, kNaN);
      continue;
    }
    const double one_plus = 1.0 + std::fabs(s.rho);
    Add(report, EssviConstraint::kLee, i, 1.0 - (s.psi * one_plus / 4.0));
    Add(report, EssviConstraint::kGatheralJacquier, i,
        1.0 - (s.psi * s.psi * one_plus / (4.0 * s.theta)));
  }
  for (std::size_t i = 1; i < slices.size(); ++i) {
    const EssviSlice<double>& lo = slices[i - 1];
    const EssviSlice<double>& hi = slices[i];
    if (!ValidSlice(lo) || !ValidSlice(hi)) continue;
    Add(report, EssviConstraint::kThetaOrder, i, (hi.theta / lo.theta) - 1.0);
    const double up = (hi.psi * (1.0 + hi.rho)) / (lo.psi * (1.0 + lo.rho)) - 1.0;
    const double down = (hi.psi * (1.0 - hi.rho)) / (lo.psi * (1.0 - lo.rho)) - 1.0;
    Add(report, EssviConstraint::kPsiLower, i, std::min(up, down));
    Add(report, EssviConstraint::kThetaRatio, i, 1.0 - ((hi.psi * lo.theta) / (lo.psi * hi.theta)));
  }
  return report;
}

EssviGlobalPoint ToGlobal(std::span<const EssviSlice<double>> slices) {
  EssviGlobalPoint out;
  const std::size_t n = slices.size();
  if (n == 0 || n > kMaxEssviSlices) return out;
  for (const EssviSlice<double>& s : slices) {
    if (!std::isfinite(s.theta) || !std::isfinite(s.psi) || !std::isfinite(s.rho)) return out;
  }
  if (!Positive(slices[0].theta)) return out;
  std::vector<double>& x = out.x;
  x.assign(3 * n, 0.0);
  const auto clamp = [&](EssviCoordinate coordinate, std::size_t slice, double value, double lo,
                         double hi) {
    const double v = std::clamp(value, lo, hi);
    if (v != value) {
      out.clamps.push_back(
          {.coordinate = coordinate, .slice = slice, .value = value, .clamped = v});
    }
    return v;
  };
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = clamp(EssviCoordinate::kRho, i, slices[i].rho, -kRhoBound, kRhoBound);
  }
  x[n] = slices[0].theta;
  double theta = slices[0].theta;
  for (std::size_t i = 1; i < n; ++i) {
    const double p = std::max((1.0 + x[i - 1]) / (1.0 + x[i]), (1.0 - x[i - 1]) / (1.0 - x[i]));
    const double a = clamp(EssviCoordinate::kA, i, slices[i].theta - (theta * p), 0.0,
                           std::numeric_limits<double>::infinity());
    x[n + i] = a;
    theta = (theta * p) + a;
  }
  // c_i needs psi_{i-1} as the map rebuilds it, so slice by slice.
  std::vector<PsiInterval> intervals;
  for (std::size_t i = 0; i < n; ++i) {
    (void)FromGlobal<double>(x, &intervals);
    const PsiInterval& iv = intervals[i];
    const double lo = i == 0 ? kC1Lower : 0.0;
    const double width = iv.upper - iv.lower;
    // An interval no wider than rounding is empty: psi_i = A_i, a clamp unless the slice has it.
    const double tolerance = kEssviConstraintTolerance * std::max(iv.upper, iv.lower);
    if (width > tolerance) {
      x[(2 * n) + i] = clamp(EssviCoordinate::kC, i, (slices[i].psi - iv.lower) / width, lo, 1.0);
    } else {
      x[(2 * n) + i] = lo;
      if (std::fabs(slices[i].psi - iv.lower) > tolerance) {
        out.clamps.push_back({.coordinate = EssviCoordinate::kC,
                              .slice = i,
                              .value = slices[i].psi,
                              .clamped = iv.lower});
      }
    }
  }
  out.mapped = true;
  return out;
}

}  // namespace aadp::svi
