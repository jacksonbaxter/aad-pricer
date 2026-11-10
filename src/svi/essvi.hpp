// eSSVI: SSVI slices with their own correlation (Hendriks and Martini, "The Extended SSVI
// Volatility Surface", 2017), the arbitrage-free comparison to the raw SVI fit (BENCHMARKS.md
// SVI-9; NOTES.md, "Stage 7c"). Templated on Real, so that the slice parameters can be the tape's
// or a Dual's inputs; nothing here branches on anything but the values.
//
// A slice, in the (theta, rho, psi) form of Mingone, arXiv 2204.00312, section 2, with psi =
// theta phi (Corbetta et al., arXiv 1804.04924, section 2.1, eq. (1)):
//   w(k) = (theta + rho psi k + sqrt((psi k + theta rho)^2 + theta^2 (1 - rho^2))) / 2
// theta is the at-the-money total variance, w(0) = theta.
//
// As raw SVI (Mingone, section 2.2.2):
//   a = theta (1 - rho^2) / 2,  b = psi / 2,  rho,  m = -theta rho / psi,
//   sigma = theta sqrt(1 - rho^2) / psi,
// defined for theta > 0, psi > 0 and |rho| < 1.
//
// The constraints, for slices i = 1..N at increasing expiries (NOTES.md, "Stage 7c"):
//   butterfly, each slice (Gatheral and Jacquier, arXiv 1204.0646, Theorem 4.2; Corbetta eqs.
//   (2), (3)):   B1  psi (1 + |rho|) < 4      B2  psi^2 (1 + |rho|) <= 4 theta
//   calendar, each adjacent pair (Hendriks and Martini, Proposition 3.5, as Mingone section 2.1
//   restates it; Pasquazzi, arXiv 2304.02106, condition N and the first branch of condition S):
//     N  theta_2 >= theta_1 and psi_2 >= psi_1 max((1 + rho_1) / (1 + rho_2),
//                                                   (1 - rho_1) / (1 - rho_2))
//     S  psi_2 / psi_1 <= theta_2 / theta_1
// Corbetta's section 2.2 states N alone as necessary and sufficient; Mingone and Pasquazzi
// disagree, and S is what they add (NOTES.md, "SVI").
//
// The global parametrisation of Mingone, section 3.2, eqs. (4) to (6), with f = f_GJ, maps every
// point of a box onto slices that satisfy all of the above:
//   x = (rho_1..rho_N, theta_1, a_2..a_N, c_1..c_N), rho in ]-1, 1[, theta_1, a > 0, c in ]0, 1[
//   p_i = max((1 + rho_{i-1}) / (1 + rho_i), (1 - rho_{i-1}) / (1 - rho_i)),  theta_i = theta_{i-1}
//         p_i + a_i
//   f_i = min(4 / (1 + |rho_i|), sqrt(4 theta_i / (1 + |rho_i|)))
//   A_1 = 0, A_i = psi_{i-1} p_i;  C_i = min(psi_{i-1} theta_i / theta_{i-1} (i > 1), f_i,
//         f_{i+1} / p_{i+1}, ..., f_N / (p_{i+1} ... p_N));  psi_i = A_i + c_i (C_i - A_i)
// B1 is strict: f's first term is held at 4 (1 - kLeeMargin) / (1 + |rho|), so that the slope of
// the converted slice, b (1 + |rho|), stays below 2 in floating point, where stage 6a's d+ check
// is strict.
#ifndef AADP_SVI_ESSVI_HPP_
#define AADP_SVI_ESSVI_HPP_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"  // Value(double)

namespace aadp::svi {

template <class Real>
struct EssviSlice {
  Real theta{};
  Real rho{};
  Real psi{};
};

inline constexpr std::size_t kMaxEssviSlices = 8;
inline constexpr double kLeeMargin = 1e-9;

// The root is taken as hypot takes it, scaled by the larger of |u| and theta, so that neither
// square underflows where w itself is representable: unscaled, (theta rho)^2 is 0 below
// theta = 1e-154, and w(0) came out as theta / 2 (NOTES.md, "Stage 7c results", decisions after
// the pre-registration). theta must be positive.
template <class Real>
Real EssviTotalVariance(const EssviSlice<Real>& s, const Real& k) {
  const Real u = s.psi * k + s.theta * s.rho;
  const Real q = (1.0 - s.rho) * (1.0 + s.rho);
  const double uv = Value(u);
  Real scale = s.theta;
  if (uv > Value(s.theta)) {
    scale = u;
  } else if (-uv > Value(s.theta)) {
    scale = -u;
  }
  const Real ru = u / scale;
  const Real rt = s.theta / scale;
  return 0.5 * (s.theta + s.rho * s.psi * k + scale * Sqrt(ru * ru + rt * rt * q));
}

template <class Real>
RawSvi<Real> ToRawSvi(const EssviSlice<Real>& s) {
  const Real q = (1.0 - s.rho) * (1.0 + s.rho);
  return {.a = 0.5 * s.theta * q,
          .b = 0.5 * s.psi,
          .rho = s.rho,
          .m = -(s.theta * s.rho) / s.psi,
          .sigma = s.theta * Sqrt(q) / s.psi};
}

// Which of C_i's terms bounds psi_i from above (the one that is smallest).
enum class PsiCap : std::uint8_t {
  kLee,               // 4 (1 - margin) / (1 + |rho_i|): B1
  kGatheralJacquier,  // sqrt(4 theta_i / (1 + |rho_i|)): B2
  kThetaRatio,        // psi_{i-1} theta_i / theta_{i-1}: S
  kLaterSlice,        // f_j / (p_{i+1} ... p_j) for a later slice j: room for j to exist
};

[[nodiscard]] const char* PsiCapName(PsiCap cap) noexcept;

// What the map did at one slice, in doubles: the interval [A_i, C_i] that c_i spans, its cap and,
// for kLaterSlice, the slice j whose room it keeps.
struct PsiInterval {
  double lower = 0.0;
  double upper = 0.0;
  PsiCap cap = PsiCap::kGatheralJacquier;
  std::size_t cap_slice = 0;
};

namespace essvi_detail {

template <class Real>
Real Abs(const Real& x) {
  return Value(x) < 0.0 ? -x : x;
}

// The smaller (larger) of two; the first on a tie, so the choice is the same for any Real.
template <class Real>
Real Min(const Real& x, const Real& y) {
  return Value(y) < Value(x) ? y : x;
}

template <class Real>
Real Max(const Real& x, const Real& y) {
  return Value(y) > Value(x) ? y : x;
}

}  // namespace essvi_detail

// Mingone's map from the box to slices. x holds 3 N numbers in the order above; a_1's slot (x[N +
// 0] is theta_1) is the only theta. Precondition: x.size() is 3 N, 1 <= N <= kMaxEssviSlices.
// No check of the box: a point outside it gives slices outside the constraints, which
// CheckEssviConstraints reports.
template <class Real>
std::vector<EssviSlice<Real>> FromGlobal(std::span<const Real> x,
                                         std::vector<PsiInterval>* intervals = nullptr) {
  using essvi_detail::Abs;
  using essvi_detail::Max;
  using essvi_detail::Min;
  const std::size_t n = x.size() / 3;
  const std::span<const Real> rho = x.subspan(0, n);
  const std::span<const Real> c = x.subspan(2 * n, n);
  std::vector<Real> p(n, Real(1.0));
  std::vector<Real> theta(n);
  theta[0] = x[n];
  for (std::size_t i = 1; i < n; ++i) {
    p[i] = Max((1.0 + rho[i - 1]) / (1.0 + rho[i]), (1.0 - rho[i - 1]) / (1.0 - rho[i]));
    theta[i] = theta[i - 1] * p[i] + x[n + i];
  }
  std::vector<Real> f(n);
  std::vector<PsiCap> f_cap(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Real one_plus = 1.0 + Abs(rho[i]);
    const Real lee = (4.0 * (1.0 - kLeeMargin)) / one_plus;
    const Real gj = Sqrt(4.0 * theta[i] / one_plus);
    f[i] = Min(lee, gj);
    f_cap[i] = Value(gj) < Value(lee) ? PsiCap::kGatheralJacquier : PsiCap::kLee;
  }
  // room[i] = min over j > i of f_j / (p_{i+1} ... p_j), built backwards; slice n-1 has none.
  std::vector<Real> room(n);
  std::vector<std::size_t> room_slice(n, n);
  for (std::size_t i = n - 1; i-- > 0;) {
    if (i + 2 == n || Value(f[i + 1]) <= Value(room[i + 1])) {
      room[i] = f[i + 1] / p[i + 1];
      room_slice[i] = i + 1;
    } else {
      room[i] = room[i + 1] / p[i + 1];
      room_slice[i] = room_slice[i + 1];
    }
  }
  std::vector<EssviSlice<Real>> out(n);
  if (intervals != nullptr) intervals->assign(n, PsiInterval{});
  for (std::size_t i = 0; i < n; ++i) {
    Real lower(0.0);
    Real upper = f[i];
    PsiCap cap = f_cap[i];
    std::size_t cap_slice = i;
    if (i > 0) {
      lower = out[i - 1].psi * p[i];
      // theta's ratio first, so that no product of two small numbers underflows.
      const Real ratio = out[i - 1].psi * (theta[i] / theta[i - 1]);
      if (Value(ratio) <= Value(upper)) {
        upper = ratio;
        cap = PsiCap::kThetaRatio;
      }
    }
    if (room_slice[i] < n && Value(room[i]) < Value(upper)) {
      upper = room[i];
      cap = PsiCap::kLaterSlice;
      cap_slice = room_slice[i];
    }
    out[i] = {.theta = theta[i], .rho = rho[i], .psi = lower + c[i] * (upper - lower)};
    if (intervals != nullptr) {
      (*intervals)[i] = {
          .lower = Value(lower), .upper = Value(upper), .cap = cap, .cap_slice = cap_slice};
    }
  }
  return out;
}

// The constraints above on given slices, in doubles. Nothing is repaired.
enum class EssviConstraint : std::uint8_t {
  kParameters,        // theta and psi finite and positive, |rho| < 1
  kLee,               // B1, strict
  kGatheralJacquier,  // B2
  kThetaOrder,        // N's theta_i >= theta_{i-1}
  kPsiLower,          // N's psi_i >= psi_{i-1} max(...)
  kThetaRatio,        // S
};

[[nodiscard]] const char* EssviConstraintName(EssviConstraint constraint) noexcept;

// slack is dimensionless, positive inside: 1 - psi (1 + |rho|) / 4, 1 - psi^2 (1 + |rho|) /
// (4 theta), theta_i / theta_{i-1} - 1, min over both wings of psi_i (1 +- rho_i) / (psi_{i-1}
// (1 +- rho_{i-1})) - 1, and 1 - psi_i theta_{i-1} / (psi_{i-1} theta_i); NaN for kParameters.
// A constraint is violated when its slack is below -kEssviConstraintTolerance, and for the strict
// B1 when it is below +kEssviConstraintTolerance; it is active when |slack| <= kActiveSlack.
// The tolerance: each side is at most about eight correctly rounded operations from the
// parameters, so the computed slack is off by at most about 16 eps; 64 eps leaves a factor 4,
// and a real violation in the tests is 1e-6.
inline constexpr double kEssviConstraintTolerance = 64.0 * 0x1p-52;
inline constexpr double kActiveSlack = 1e-9;

struct EssviConstraintState {
  EssviConstraint constraint = EssviConstraint::kParameters;
  std::size_t slice = 0;  // for a calendar constraint, the later slice of the pair
  double slack = 0.0;
  bool active = false;
  bool violated = false;
};

struct EssviConstraintReport {
  std::vector<EssviConstraintState> states;
  std::size_t violations = 0;
};

[[nodiscard]] EssviConstraintReport CheckEssviConstraints(
    std::span<const EssviSlice<double>> slices);

// The box of the fit, per coordinate of x (NOTES.md, "Stage 7c"). rho within kRhoBound of +-1
// keeps the converted sigma = theta sqrt(1 - rho^2) / psi above 1e-8 for theta >= 1e-6 (psi <=
// 2 sqrt(theta) by B2), inside stage 6a's and 7a's validation; c_1 >= kC1Lower keeps psi_1 away
// from 0, where m and sigma of the converted slice grow without bound.
inline constexpr double kRhoBound = 1.0 - 1e-6;
inline constexpr double kC1Lower = 1e-3;

// The inverse of FromGlobal, for slices given from outside: x from (theta, rho, psi), with each
// coordinate that falls outside the box clamped to it and the clamp recorded, so that
// FromGlobal(x) is a surface inside the constraints. A slice inside them maps back to itself, to
// rounding. theta_1 not finite and positive, or psi or theta not finite, cannot be mapped.
enum class EssviCoordinate : std::uint8_t { kRho, kTheta1, kA, kC };

[[nodiscard]] const char* EssviCoordinateName(EssviCoordinate coordinate) noexcept;

// value is the coordinate as computed from the given slices and clamped the end of the box it was
// set to. Where [A_i, C_i] is empty to rounding, c_i cannot be computed: the clamp of kC then holds
// psi_i itself and A_i, the only psi_i the map allows.
struct EssviClamp {
  EssviCoordinate coordinate = EssviCoordinate::kRho;
  std::size_t slice = 0;
  double value = 0.0;
  double clamped = 0.0;
};

struct EssviGlobalPoint {
  std::vector<double> x;
  std::vector<EssviClamp> clamps;
  bool mapped = false;  // false: theta_1, or a theta or psi, not usable
};

[[nodiscard]] EssviGlobalPoint ToGlobal(std::span<const EssviSlice<double>> slices);

}  // namespace aadp::svi

#endif  // AADP_SVI_ESSVI_HPP_
