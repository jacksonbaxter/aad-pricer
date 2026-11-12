// Hand-made raw SVI surfaces for the stage 7a tests. Nothing here comes from market data.
//
// Three expiries at 5, 12 and 33 days, as in the calibration set.
//   A  free of static arbitrage: stage 6b's synthetic slices A, B and C (docs/NOTES.md,
//      "Stage 6b").
//   B  shaped like a raw SVI fit with rho near +1 and the vertex to the right of the money, so
//      that adjacent slices cross on the right and each slice has a density violation there: the
//      floors fire, as they are expected to on the fitted surface.
//   Flat  b = 0 and a_j = sigma^2 T_j: total variance sigma^2 t, so a constant local volatility.
#ifndef AADP_TESTS_SVI_SYNTHETIC_SURFACE_HPP_
#define AADP_TESTS_SVI_SYNTHETIC_SURFACE_HPP_

#include <array>
#include <cmath>

#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::svi::testing {

inline constexpr std::array<double, 3> kExpiries = {5.0 / 365.0, 12.0 / 365.0, 33.0 / 365.0};

struct Surface {
  std::array<SliceTerms, 3> terms;
  std::array<RawSvi<double>, 3> params;
};

// Forwards and discounts with a different carry and rate in each segment.
inline constexpr std::array<double, 3> kForwards = {100.02, 100.09, 100.12};
inline constexpr std::array<double, 3> kDiscounts = {0.9998, 0.9993, 0.9978};

inline Surface WithTerms(const std::array<RawSvi<double>, 3>& params,
                         const std::array<double, 3>& forwards,
                         const std::array<double, 3>& discounts) {
  Surface s{};
  for (std::size_t j = 0; j < 3; ++j) {
    s.terms[j] = {.expiry = kExpiries[j], .forward = forwards[j], .discount = discounts[j]};
  }
  s.params = params;
  return s;
}

inline constexpr std::array<RawSvi<double>, 3> kParamsA = {{
    {.a = 1e-4, .b = 0.012, .rho = -0.45, .m = 0.002, .sigma = 0.025},
    {.a = 2e-4, .b = 0.02, .rho = -0.35, .m = 0.005, .sigma = 0.04},
    {.a = 5e-4, .b = 0.04, .rho = -0.5, .m = 0.01, .sigma = 0.06},
}};

inline constexpr std::array<RawSvi<double>, 3> kParamsB = {{
    {.a = -0.0025, .b = 1.5, .rho = 0.985, .m = 0.09, .sigma = 0.012},
    {.a = -0.002, .b = 1.5, .rho = 0.985, .m = 0.12, .sigma = 0.013},
    {.a = -0.05, .b = 1.6, .rho = 0.96, .m = 0.45, .sigma = 0.12},
}};

inline Surface SurfaceA() { return WithTerms(kParamsA, kForwards, kDiscounts); }
inline Surface SurfaceB() { return WithTerms(kParamsB, kForwards, kDiscounts); }
// A with F = 100 and D = 1 at every slice, for Dupire in call prices.
inline Surface SurfaceAFlatForward() {
  return WithTerms(kParamsA, {100.0, 100.0, 100.0}, {1, 1, 1});
}

inline constexpr double kFlatVol = 0.2;
inline constexpr double kFlatRate = 0.02;
inline constexpr double kFlatSpot = 100.0;

// sigma = kFlatVol, F_j = S0 exp(r T_j), D_j = exp(-r T_j), no dividend, so S0 = D_0 F_0 up to
// rounding. The SVI rho, m and sigma do not matter when b = 0.
inline Surface FlatSurface() {
  Surface s{};
  for (std::size_t j = 0; j < 3; ++j) {
    const double t = kExpiries[j];
    s.terms[j] = {.expiry = t,
                  .forward = kFlatSpot * std::exp(kFlatRate * t),
                  .discount = std::exp(-kFlatRate * t)};
    s.params[j] = {.a = kFlatVol * kFlatVol * t, .b = 0.0, .rho = -0.3, .m = 0.01, .sigma = 0.1};
  }
  return s;
}

}  // namespace aadp::svi::testing

#endif  // AADP_TESTS_SVI_SYNTHETIC_SURFACE_HPP_
