// Raw SVI fit to one slice of option prices (BENCHMARKS.md SVI-6, SVI-7, SVI-9, SVI-11), as
// pre-registered in NOTES.md, "Stage 6b".
//
// Objective: Phi = sum r_i^2, r_i = (V_model,i - V_i) / noise_i, V_model the Black-76 price at the
// total variance w(k_i) of raw SVI, k_i = ln(K_i / F).
//
// Method: the quasi-explicit form of De Marco and Martini (Zeliade, 2009). For fixed (m, sigma),
// with y = (k - m) / sigma, w = a + d y + c sqrt(y^2 + 1), c = b sigma, d = rho b sigma, linear in
// (a, d, c). The inner solve is Gauss-Newton on Phi in (a, d, c): each step is the weighted linear
// least squares sum J_i^2 (t_i - w_i)^2, J_i = (dV_i/dw) / noise_i, t_i = w_i - r_i / J_i, a convex
// quadratic programme in three variables under the linear constraints
//   |d| <= c                (|rho| <= 1, so b >= 0)
//   c + |d| <= 4 sigma      (b (1 + |rho|) <= 4, Zeliade's bound, not Lee's 2)
//   a <= max_i w_market,i
// solved exactly by trying every set of at most three active constraints. The first step is at
// the market total variances, where r = 0: the classical vega-weighted quasi-explicit solve. Its
// fixed point is stationary for Phi itself. Positivity of the minimum variance,
// a + b sigma sqrt(1 - rho^2) > 0, replaces Zeliade's a >= 0 and is held by backtracking.
// The outer search is a 9 x 9 grid in (m, sigma), then Nelder-Mead in (m, ln sigma) from the 5
// best grid points, inside the box m in [k_min - D, k_max + D], D = k_max - k_min, sigma in
// [1e-4, 4]. Deterministic: no seed.
//
// Nothing here repairs a slice: the parameters are returned as fitted, and the arbitrage checks
// (arbitrage.hpp) run on them unchanged. Every error is a value; nothing throws.
#ifndef AADP_SVI_FIT_HPP_
#define AADP_SVI_FIT_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "svi/arbitrage.hpp"
#include "svi/implied_vol.hpp"

namespace aadp::svi {

struct FitQuote {
  double strike = 0.0;
  double price = 0.0;  // Discounted, as quoted.
  OptionType type = OptionType::kCall;
  double noise = 1.0;  // sigma_i, in price units.
};

struct FitSlice {
  double forward = 0.0;
  double discount = 1.0;
  double expiry = 0.0;  // T in years.
  std::vector<FitQuote> quotes;
};

// The defaults are the pre-registered fit. The fuzzer uses a smaller search, with the same code.
struct FitOptions {
  std::size_t grid_m = 9;
  std::size_t grid_sigma = 9;
  double grid_sigma_lo = 1e-3;
  double grid_sigma_hi = 1.0;
  std::size_t starts = 5;
  std::size_t max_evaluations_per_start = 600;
  double simplex_tolerance = 1e-10;  // In m and in ln sigma.
  std::size_t max_inner_steps = 50;
  double inner_relative_decrease = 1e-13;
  double sigma_lo = 1e-4;
  double sigma_hi = 4.0;
  double slope_bound = 4.0;  // b (1 + |rho|) <= slope_bound.
};

// Each constraint of the fit, in the order of FitResult::bounds.
enum class Bound : std::uint8_t {
  kRhoLower,  // rho = -1
  kRhoUpper,  // rho = +1
  kBZero,     // b = 0
  kSlope,     // b (1 + |rho|) = slope_bound
  kAUpper,    // a = max market total variance
  kMLower,    // m at either end of its box
  kMUpper,
  kSigmaLower,  // sigma at either end of its box
  kSigmaUpper,
  kMinVariance,  // a + b sigma sqrt(1 - rho^2) = 0
};
inline constexpr std::size_t kBoundCount = 10;

[[nodiscard]] const char* BoundName(Bound bound) noexcept;

// A constraint is active when its slack is at most kActiveTolerance of its scale (NOTES.md,
// "Stage 6b"). The slack is in natural units: 1 + rho, 1 - rho, b, slope_bound - b (1 + |rho|),
// a_max - a, m - m_lo, m_hi - m, ln(sigma / sigma_lo), ln(sigma_hi / sigma), and the minimum
// variance.
inline constexpr double kActiveTolerance = 1e-9;

struct BoundState {
  Bound bound = Bound::kRhoLower;
  double slack = 0.0;
  bool active = false;
};

struct FitResult {
  SviParams params;
  double objective = 0.0;  // Phi
  std::size_t quotes = 0;
  // The best start stopped on its tolerance, and the inner solve at its point converged.
  bool converged = false;
  std::array<BoundState, kBoundCount> bounds{};
  double m_lo = 0.0;
  double m_hi = 0.0;
  double sigma_lo = 0.0;
  double sigma_hi = 0.0;
  double a_max = 0.0;
  std::size_t starts = 0;
  std::size_t starts_at_best = 0;  // Starts within 1e-8 of the best Phi, relatively.
  std::size_t profile_evaluations = 0;
  // Per quote, in the slice's order.
  std::vector<double> market_vol;
  std::vector<double> model_vol;
  std::vector<double> model_price;
  std::vector<double> residual;  // r_i

  [[nodiscard]] std::vector<Bound> ActiveBounds() const;
};

enum class FitErrorCode : std::uint8_t {
  kBadSlice,         // F, D or T not finite and positive.
  kBadQuote,         // A strike, price or noise not finite and positive.
  kTooFewStrikes,    // Fewer than 5 distinct strikes.
  kNoImpliedVol,     // A quote that does not invert; status says why.
  kNoFeasibleStart,  // No grid point gave a finite objective.
};

struct FitError {
  FitErrorCode code = FitErrorCode::kBadSlice;
  std::size_t quote = 0;  // The offending quote, for kBadQuote and kNoImpliedVol.
  InversionStatus status = InversionStatus::kOk;
};

[[nodiscard]] const char* FitErrorName(FitErrorCode code) noexcept;

[[nodiscard]] std::expected<FitResult, FitError> FitRawSvi(const FitSlice& slice,
                                                           const FitOptions& options = {});

// Phi at given parameters; NaN if a quote is invalid or w <= 0 at a quote.
[[nodiscard]] double Objective(const FitSlice& slice, const SviParams& params);

// (J^T J)^-1, J the Jacobian of r in (a, b, rho, m, sigma), from the analytic derivatives of w.
// With unit-variance noise in r it is the parameters' covariance to first order. Empty where
// w <= 0 at a quote or J^T J is singular.
using Matrix5 = std::array<std::array<double, 5>, 5>;
[[nodiscard]] std::optional<Matrix5> ParameterCovariance(const FitSlice& slice,
                                                         const SviParams& params);

// SVI-6: (1/n) sum |V_model,i - V_i| / F, in basis points of the forward. Unweighted.
[[nodiscard]] double MeanAbsPriceErrorBp(const FitSlice& slice,
                                         std::span<const double> model_price);

// SVI-11, Zhang, Lin and Lu: sqrt((1/n) sum (sigma_model,i - sigma_market,i)^2), in vol points
// (100 times the decimal). Unweighted.
[[nodiscard]] double ImpliedVolRmse(std::span<const double> model_vol,
                                    std::span<const double> market_vol);

}  // namespace aadp::svi

#endif  // AADP_SVI_FIT_HPP_
