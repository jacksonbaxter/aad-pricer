// Sensitivities of a fitted raw SVI slice to its market quotes, by the implicit function theorem
// at the constrained optimum of the fit (BENCHMARKS.md SVI-10; NOTES.md, "Stage 7b").
//
// The fit (fit.hpp) minimises Phi(theta; V, F, D, s) = sum_i r_i^2, r_i = (B_i(theta) - V_i) /
// noise_i, over theta = (a, b, rho, m, sigma), with B_i the Black-76 price at the raw SVI total
// variance, noise_i = sqrt(s^2 + tau_i^2), s = sigma_leg and tau_i^2 = tick_i^2 / 12. At its
// optimum some constraints c_k(theta) <= 0 may be active. Then the KKT conditions
//   grad Phi + sum_k mu_k grad c_k = 0,  c_k = 0,  mu_k >= 0
// hold, and differentiating them in an input p (a quote V_i, or F, D or s) gives
//   [ H_L      grad c ] [ d theta / dp ]   [ -d2 Phi / d theta dp ]
//   [ grad c^T   0    ] [ d mu / dp    ] = [          0           ],  H_L = H + sum_k mu_k Hess c_k
// with H the Hessian of Phi. The solution lies on the constraint manifold (grad c . d theta = 0).
// It is the derivative only under strict complementarity (mu_k > 0 for every active k), which is
// checked: a multiplier indistinguishable from zero is an error value, because then a small move
// of the quotes can release the constraint and the derivative is one-sided.
//
// Constraints, in raw parameters, written c <= 0: the slope b (1 + |rho|) - L (L the fit's slope
// bound; in the inner variables c + |d| = L sigma), rho - 1 and -1 - rho. The slope's Hessian has
// d2 c / db drho = sign(rho), which H_L must carry in these coordinates. Any other active bound
// is refused as an error value.
//
// Exact Hessian. H, d2 Phi / d theta dV_i and d2 Phi / d theta d(F, D, s) come from one record of
// Phi on the second-order tape (ad/second_order.hpp) over Dual<double, 8>, tangents seeded on
// (theta, F, D, s), and one reverse sweep: forward over reverse. H includes the residual-curvature
// term 2 sum_i r_i Hess r_i. The Gauss-Newton version replaces every second derivative of Phi by
// 2 J^T J, J the Jacobian of r in (theta, F, D, s), read from the same record's tangents. The
// quote block is the same for both, since r_i is linear in V_i with noise_i not depending on it.
//
// Black-76 on the tape is D (F N(d1) - K N(d2)) for a call and D (K N(-d2) - F N(-d1)) for a put,
// on the pricer's normal CDF, not Let's Be Rational: its value differs from the fit's price by
// rounding only.
//
// Scaling. Each parameter is scaled by 1 / sqrt of the Gauss-Newton diagonal, so that every entry
// of the KKT matrix is O(1); the condition number reported and limited is that of the scaled
// matrix, in the 1-norm. Every failure is a value; nothing throws.
#ifndef AADP_SVI_CALIBRATION_ADJOINT_HPP_
#define AADP_SVI_CALIBRATION_ADJOINT_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "svi/fit.hpp"

namespace aadp::svi {

inline constexpr std::size_t kSviParameters = 5;
// The inputs of Phi other than theta and the quotes, in this order.
inline constexpr std::size_t kMarketInputs = 3;
enum class MarketInput : std::uint8_t { kForward, kDiscount, kSigmaLeg };

using Vector5 = std::array<double, kSviParameters>;

struct ObjectiveDerivatives {
  double objective = 0.0;  // Phi, by the tape's Black-76
  Vector5 gradient{};      // d Phi / d theta
  Matrix5 hessian{};       // exact, symmetrised
  Matrix5 gauss_newton{};  // 2 J^T J
  // max |H_jk - H_kj| / sqrt(GN_jj GN_kk) before symmetrising: rounding only.
  double hessian_asymmetry = 0.0;
  std::vector<Vector5> quote_cross;                   // d2 Phi / d theta d V_i, per quote
  std::array<Vector5, kMarketInputs> market_cross{};  // d2 Phi / d theta d(F, D, s), exact
  std::array<Vector5, kMarketInputs> market_cross_gauss_newton{};
};

enum class AdjointErrorCode : std::uint8_t {
  kBadSlice,              // F, D or T not finite and positive
  kBadQuote,              // a strike, price or noise not finite and positive, or noise < s
  kBadSigmaLeg,           // s not finite, or negative
  kBadParameters,         // not finite, b <= 0, |rho| > 1, sigma <= 0
  kNonPositiveVariance,   // w <= 0 at a quote
  kNonFinite,             // a derivative came out NaN or infinite
  kUnsupportedBound,      // an active bound other than the slope or rho = -1 or +1
  kDegenerateMultiplier,  // a multiplier not clearly positive: no strict complementarity
  kNotMinimum,            // the reduced Hessian is not positive definite
  kSingular,              // a zero pivot in the scaled KKT matrix
  kIllConditioned,        // its condition number above AdjointOptions::max_condition
};

struct AdjointError {
  AdjointErrorCode code = AdjointErrorCode::kBadSlice;
  std::size_t quote = 0;        // for kBadQuote and kNonPositiveVariance
  Bound bound = Bound::kSlope;  // for kUnsupportedBound and kDegenerateMultiplier
  double value = 0.0;           // the condition number, or the multiplier's margin
};

[[nodiscard]] const char* AdjointErrorName(AdjointErrorCode code) noexcept;

// Phi's derivatives at theta. sigma_leg = 0 means the noise does not depend on s (its row is 0).
[[nodiscard]] std::expected<ObjectiveDerivatives, AdjointError> DifferentiateObjective(
    const FitSlice& slice, const SviParams& params, double sigma_leg = 0.0);

// The defaults are NOTES.md, "Stage 7b".
struct AdjointOptions {
  // Above this, the scaled KKT matrix loses more than about 2^-52 kappa of relative accuracy.
  double max_condition = 0x1p40;
  // A multiplier must exceed this many times its numerical uncertainty (ActiveConstraint::margin).
  double min_multiplier_margin = 10.0;
};

inline constexpr std::size_t kMaxActiveConstraints = 3;
using Multipliers = std::array<double, kMaxActiveConstraints>;

// Multipliers. A fitted point is stationary only to the fit's tolerance, and in an ill-conditioned
// problem the gradient left over lies mostly along the stiff directions, so the least-squares
// multiplier from the gradient alone carries a first-order error. One Newton step on the KKT
// system from the fitted point, [H_L A^T; A 0] [delta; mu] = [-grad Phi; 0], removes it: delta
// estimates the distance to the exact KKT point and mu is the multiplier there, correct to second
// order. A second Newton step, from theta + delta, measures what is left.
struct ActiveConstraint {
  Bound bound = Bound::kSlope;
  double multiplier = 0.0;  // mu from the first Newton step, in units of Phi per unit of c
  double multiplier_least_squares = 0.0;  // from the gradient at the fitted point alone
  double multiplier_second_step = 0.0;    // from the second Newton step
  // mu / |mu - mu_second_step| (mu / |mu - mu_least_squares| if the second step cannot be taken):
  // how many times mu exceeds its numerical uncertainty.
  double margin = 0.0;
  // mu / sqrt(sum_i (d mu / d V_i noise_i)^2): mu over its standard error under the fit's own
  // noise model, so how far from zero the quotes' noise could plausibly move it.
  double significance = 0.0;
  // min_i mu / (|d mu / d V_i| noise_i): the fewest noise units one quote alone must move to
  // release the bound, to first order.
  double quote_distance = 0.0;
};

// One Hessian's solution.
struct ParameterSensitivity {
  std::vector<Vector5> quote;                   // d theta* / d V_i, per quote
  std::array<Vector5, kMarketInputs> market{};  // d theta* / d(F, D, s)
  std::vector<Multipliers> quote_multiplier;    // d mu_k / d V_i, per quote
  std::array<Multipliers, kMarketInputs> market_multiplier{};
  double condition = 0.0;          // of the scaled KKT matrix, 1-norm
  double manifold_residual = 0.0;  // max over inputs |S grad c . d theta| / |S^-1 d theta|
};

struct CalibrationAdjoint {
  ObjectiveDerivatives derivatives;
  std::vector<ActiveConstraint> constraints;
  double stationarity = 0.0;   // |S (grad Phi + sum mu_least_squares grad c)|
  double gradient_norm = 0.0;  // |S grad Phi|
  Vector5 newton_step{};       // delta: the exact KKT point less the fitted one, estimated
  ParameterSensitivity exact;
  // Gauss-Newton: the same system with every second derivative of Phi replaced by 2 J^T J.
  std::expected<ParameterSensitivity, AdjointError> gauss_newton = std::unexpected(AdjointError{});
};

// The adjoint at params, with the bounds listed in `active` holding with equality. The slope
// constraint's gradient and Hessian do not depend on L, so L is not an argument.
[[nodiscard]] std::expected<CalibrationAdjoint, AdjointError> CalibrationAdjointAt(
    const FitSlice& slice, const SviParams& params, std::span<const Bound> active,
    double sigma_leg = 0.0, const AdjointOptions& options = {});

// The same at a fit's parameters and active bounds.
[[nodiscard]] std::expected<CalibrationAdjoint, AdjointError> CalibrationAdjointOf(
    const FitSlice& slice, const FitResult& fit, double sigma_leg = 0.0,
    const AdjointOptions& options = {});

}  // namespace aadp::svi

#endif  // AADP_SVI_CALIBRATION_ADJOINT_HPP_
