// The eSSVI surface fit to the same quotes, weights and objective as the raw SVI fit of stage 6b,
// all slices at once (BENCHMARKS.md SVI-9; NOTES.md, "Stage 7c", pre-registered before any fit
// on the real chain).
//
// Objective: Phi = sum over slices j and quotes i of r_ji^2, r_ji = (V_model,ji - V_ji) / noise_ji,
// V_model the Black-76 price at the slice's eSSVI total variance (essvi.hpp), k = ln(K / F_j):
// stage 6b's Phi per slice, summed.
//
// Coordinates: Mingone's global parametrisation (essvi.hpp, FromGlobal), so that every point of
// the box is a surface free of butterfly and calendar arbitrage by construction. The box:
//   rho_i in [-kRhoBound, kRhoBound],  theta_1 in [1e-3 w_min,1, 10 w_max,1],
//   a_i in [0, 10 w_max,i],  c_1 in [kC1Lower, 1],  c_i in [0, 1] for i > 1,
// with w_min,j and w_max,j the smallest and largest market total variance of slice j.
//
// Optimiser: Levenberg-Marquardt on the box, with Marquardt's diagonal scaling. The Jacobian is
// exact: dV/dw = D F phi(d1) / (2 sqrt(w)) times dw/dx, which a forward-mode Dual carries through
// the map and the slice. A coordinate at an end of the box whose gradient points out of it is held
// there for the step; every trial point is clamped to the box. The damping lambda starts at 1e-3,
// falls by 3 after a step that lowers Phi and grows by 4 after one that does not.
// A start stops, converged, when
//   - the scaled projected gradient max_j |g_j| / sqrt(A_jj Phi) over the free coordinates, g =
//     J^T r and A = J^T J, is at most gradient_tolerance (first-order stationarity, scale-free,
//     at most 1 by Cauchy-Schwarz),
//   - no lambda up to lambda_max lowers Phi, or
//   - slow_steps accepted steps in a row each lower Phi by at most slow_decrease of itself;
// and, not converged, after max_iterations steps.
// Starts: Mingone's (section 4.2: rho_i = 0, c_i = 1/2, theta_1 the market's at-the-money total
// variance theta*_1, a_i = max(theta*_i - theta*_{i-1}, 0)), then random_starts more, uniform in a
// stated part of the box from Philox (seed, start, coordinate). The result is the lowest Phi.
//
// Nothing is repaired, and every error is a value.
#ifndef AADP_SVI_ESSVI_FIT_HPP_
#define AADP_SVI_ESSVI_FIT_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "svi/arbitrage.hpp"
#include "svi/essvi.hpp"
#include "svi/fit.hpp"

namespace aadp::svi {

// The defaults are the pre-registered fit. The fuzzer uses fewer starts and steps, with the same
// code. The seed is a development seed, never 1729 or 271828.
struct EssviFitOptions {
  std::size_t random_starts = 15;
  std::uint64_t seed = 20261121;
  std::size_t max_iterations = 2000;
  double gradient_tolerance = 1e-9;
  double slow_decrease = 1e-14;
  std::size_t slow_steps = 10;
  double lambda_start = 1e-3;
  double lambda_max = 1e16;
};

enum class EssviStop : std::uint8_t {
  kStationary,
  kNoDecrease,
  kSlow,
  kIterations,  // not converged
};

[[nodiscard]] const char* EssviStopName(EssviStop stop) noexcept;

struct EssviCoordinateState {
  EssviCoordinate coordinate = EssviCoordinate::kRho;
  std::size_t slice = 0;
  double value = 0.0;
  double lower = 0.0;
  double upper = 0.0;
  // At an end of the box, within kActiveTolerance of its width.
  bool at_lower = false;
  bool at_upper = false;
};

struct EssviSliceFit {
  EssviSlice<double> params;
  SviParams raw;  // ToRawSvi(params)
  PsiInterval interval;
  double objective = 0.0;  // this slice's Phi
  std::size_t quotes = 0;
  std::vector<double> market_vol;
  std::vector<double> model_vol;
  std::vector<double> model_price;
  std::vector<double> residual;
};

struct EssviStart {
  double objective = 0.0;
  EssviStop stop = EssviStop::kIterations;
  std::size_t iterations = 0;
  double gradient = 0.0;  // the scaled projected gradient where it stopped
};

struct EssviFitResult {
  std::vector<double> x;  // the box coordinates, FromGlobal's order
  std::vector<EssviSliceFit> slices;
  std::vector<EssviCoordinateState> coordinates;  // in x's order
  EssviConstraintReport constraints;              // CheckEssviConstraints on the fitted slices
  double objective = 0.0;                         // Phi, all slices
  bool converged = false;
  EssviStop stop = EssviStop::kIterations;
  std::size_t iterations = 0;
  double gradient = 0.0;
  std::size_t starts_at_best = 0;  // within 1e-8 of the best Phi, relatively
  std::vector<EssviStart> starts;  // in start order; Mingone's first
};

enum class EssviFitErrorCode : std::uint8_t {
  kSliceCount,       // none, or more than kMaxEssviSlices
  kExpiryOrder,      // expiries not strictly increasing
  kSlice,            // one slice fails as FitRawSvi would: fit says how
  kNoFeasibleStart,  // no start gave a finite Phi
};

struct EssviFitError {
  EssviFitErrorCode code = EssviFitErrorCode::kSliceCount;
  std::size_t slice = 0;
  FitError fit;
};

[[nodiscard]] const char* EssviFitErrorName(EssviFitErrorCode code) noexcept;

// Slices in order of expiry. Each needs what FitRawSvi needs: F, D, T finite and positive, every
// quote valid and inverting, at least 5 distinct strikes.
[[nodiscard]] std::expected<EssviFitResult, EssviFitError> FitEssviSurface(
    std::span<const FitSlice> slices, const EssviFitOptions& options = {});

// Phi at a box point x, all slices; NaN if a slice or quote is invalid. For tests and the fuzzer.
[[nodiscard]] double EssviObjective(std::span<const FitSlice> slices, std::span<const double> x);

// (J^T J)^-1, J the exact Jacobian of r in the slice parameters (theta_j, rho_j, psi_j), j in
// slice order: 3 N x 3 N, row-major. Empty where a quote is invalid or J^T J is singular. With
// unit-variance noise in r, the parameters' covariance to first order, ignoring the constraints.
[[nodiscard]] std::vector<double> EssviParameterCovariance(
    std::span<const FitSlice> slices, std::span<const EssviSlice<double>> params);

}  // namespace aadp::svi

#endif  // AADP_SVI_ESSVI_FIT_HPP_
