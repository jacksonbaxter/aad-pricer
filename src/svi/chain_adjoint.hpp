// Quote sensitivities of the fitted TAIFEX chain, and the pieces of the bump-and-recalibrate check
// (docs/NOTES.md, "Stage 7b"). The inputs are settlements, not tradable quotes.
//
// Two chains from a settlement V_i of a calibration quote to the fitted parameters:
//   market fixed    F, D and sigma_leg held at the parity regression's values: d theta* / d V_i
//                   from calibration_adjoint.hpp alone
//   through parity  V_i is also one leg of the parity point C - P at its strike, so the parity
//                   regression's F, D and sigma_leg move with it, and with sigma_leg every noise_i:
//                   d theta* / d V_i + sum_p d theta* / d p  d p / d V_i, p in (F, D, sigma_leg)
// In both, the calibration set's membership and every quote's tick are held fixed: both are
// piecewise constant in the settlements, so their derivative is zero almost everywhere.
//
// Parity (data/forwards.hpp): ordinary least squares of y_j = C_j - P_j on K_j, slope beta,
// intercept alpha, D = -beta, F = alpha / D, sigma_leg = sqrt(sum e_j^2 / n) / sqrt 2. Linear in y
// for alpha and beta, so
//   d beta / d y_j = (K_j - K_bar) / S_KK,  d alpha / d y_j = 1/n - K_bar (K_j - K_bar) / S_KK,
//   d D / d y_j = -d beta / d y_j,  d F / d y_j = (d alpha / d y_j - F d D / d y_j) / D,
//   d sigma_leg / d y_j = e_j / (n RMS sqrt 2), since the residuals e = (I - hat) y and
//   (I - hat) e = e.
#ifndef AADP_SVI_CHAIN_ADJOINT_HPP_
#define AADP_SVI_CHAIN_ADJOINT_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "data/calibration_set.hpp"
#include "data/forwards.hpp"
#include "svi/calibration_adjoint.hpp"
#include "svi/chain_fit.hpp"
#include "svi/fit.hpp"

namespace aadp::svi {

using MarketVector = std::array<double, kMarketInputs>;

// d(F, D, sigma_leg) / d y_j for each parity point, in the points' order.
struct ParityJacobian {
  std::vector<double> strike;
  std::vector<MarketVector> d_market;
};

enum class ParityJacobianError : std::uint8_t {
  kParity,    // fit_parity refused the points
  kExactFit,  // zero residuals: sigma_leg = 0 has no derivative
};

[[nodiscard]] std::expected<ParityJacobian, ParityJacobianError> DifferentiateParity(
    std::span<const ParityPoint> points);

// Calibration quote i's settlement as a leg of the parity point at its strike: y_j moves by
// +V_i for a call and -V_i for a put. A strike with no parity point has no leg.
struct QuoteLeg {
  bool present = false;
  std::size_t point = 0;
  double sign = 1.0;
};

[[nodiscard]] std::vector<QuoteLeg> QuoteLegs(const CalibrationSlice& rows,
                                              std::span<const ParityPoint> points);

enum class QuoteChain : std::uint8_t { kMarketFixed, kThroughParity };

[[nodiscard]] const char* QuoteChainName(QuoteChain chain) noexcept;

// The slice with quote i's settlement moved by h, for the bump check. kMarketFixed moves V_i
// alone. kThroughParity also moves its parity leg, refits the regression, and rebuilds F, D and
// every noise_i = sqrt(sigma_leg^2 + tau_i^2) with tau_i^2 = noise_i^2 - sigma_leg^2 held.
struct BumpedSlice {
  FitSlice quotes;
  ParityFit parity;
};

[[nodiscard]] std::expected<BumpedSlice, ParityError> BumpQuote(const ChainSlice& slice,
                                                                std::span<const ParityPoint> points,
                                                                std::span<const QuoteLeg> legs,
                                                                std::size_t quote, double h,
                                                                QuoteChain chain);

enum class ChainAdjointErrorCode : std::uint8_t {
  kFit,      // the slice did not fit
  kParity,   // the parity regression or its derivative failed
  kLegs,     // legs and quotes of different sizes
  kAdjoint,  // adjoint says why
};

struct ChainAdjointError {
  ChainAdjointErrorCode code = ChainAdjointErrorCode::kFit;
  AdjointError adjoint;
};

struct SliceQuoteSensitivity {
  CalibrationAdjoint adjoint;  // the market-fixed chain, both Hessians
  std::vector<QuoteLeg> legs;
  std::vector<MarketVector> market;                  // d(F, D, sigma_leg) / d V_i through parity
  std::vector<Vector5> through_parity;               // exact Hessian
  std::vector<Vector5> through_parity_gauss_newton;  // empty if Gauss-Newton failed
};

// Both chains for one fitted slice.
[[nodiscard]] std::expected<SliceQuoteSensitivity, ChainAdjointError> SliceQuoteSensitivities(
    const ChainSlice& slice, std::span<const ParityPoint> points, const FitResult& fit,
    const AdjointOptions& options = {});

// The halving rule of the bump check. differences[l] is a central difference at h[l], h halving
// with l; usable[l] says whether level l counts (both bumped fits converged with the unbumped
// active set). For each usable l with a finer usable level, the error left in differences[l] by
// truncation and by events inside the bump (a path crossing a kink of the payoff or of the floor
// rule) is estimated as the largest of its distances from every finer usable level: on an h^2
// error that is at least 3/4 of the error, on an h error at least 1/2, and a sequence that
// settles only at small h (the bridge's sharp crossing probability near the barrier) is not
// judged by a coarse level's neighbours. The refit error is refit_error / h[l] (each of the two
// refits off by up to refit_error). The level with the smallest sum is judged: pass if
// |differences[l] - reference| <= kHalvingFactor (truncation + refit) + floor. With no such
// level the comparison is unresolved: a failure.
inline constexpr double kHalvingFactor = 4.0;

struct HalvingJudgement {
  bool resolved = false;
  std::size_t level = 0;
  double estimate = 0.0;
  double truncation = 0.0;
  double refit = 0.0;
  double tolerance = 0.0;
  double error = 0.0;
  bool pass = false;
};

// The level-selection half of the rule alone, for a derivative with no reference to judge it by:
// resolved, level, estimate, truncation and refit as above.
[[nodiscard]] HalvingJudgement SelectByHalving(std::span<const double> h,
                                               std::span<const double> differences,
                                               std::span<const bool> usable, double refit_error);

[[nodiscard]] HalvingJudgement JudgeByHalving(std::span<const double> h,
                                              std::span<const double> differences,
                                              std::span<const bool> usable, double refit_error,
                                              double reference, double floor);

}  // namespace aadp::svi

#endif  // AADP_SVI_CHAIN_ADJOINT_HPP_
