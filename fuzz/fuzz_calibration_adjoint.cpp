// The KKT / implicit-function entry point (src/svi/calibration_adjoint.hpp) on small synthetic
// slices from raw bytes, every field free to take any value, NaN and infinities included:
//
//   offset  0  u8          quote count, mod 13 (0 to 12), cut to the quotes the input holds
//   offset  1  u8          active set: bit 0 slope, bit 1 rho = -1, bit 2 rho = +1, bit 3 m lower
//   offset  2  u16         put mask: bit i set makes quote i a put
//   offset  4  u8          bit 0: fit first (a small search) and differentiate at the fit
//   offset  5  u8 x 3      unused
//   offset  8  f64 x 3     forward, discount, expiry
//   offset 32  f64 x 6     a, b, rho, m, sigma, sigma_leg
//   offset 80  f64 x 3n    per quote: strike, price, noise
//   little-endian. An input shorter than 80 bytes is not a case.
//
// Properties. Nothing throws, crashes or trips a sanitizer, and the result is the adjoint or an
// explicit error value:
// - an error has a name and says what the input shows: a bad slice exactly when F, D or T is not
//   finite and positive; a bad sigma_leg only when it is not finite and non-negative; a bad quote
//   at an index inside the slice that is bad or below sigma_leg (or an empty slice); bad
//   parameters only when they are not finite or b <= 0, |rho| > 1 or sigma <= 0; an unsupported
//   bound only with one in the set it refuses; an ill-conditioned matrix only above the limit.
// - an adjoint has one entry per quote, every number finite, one constraint per active bound
//   with mu > 0 and margin >= the option's minimum, a condition number in [1, max_condition],
//   and each solution solves its KKT system: the scaled residual of
//   [H_L A^T; A 0] [d theta; d mu] = [-cross; 0], with H_L and A built here from the
//   constraints' formulas, is within 2^-36 (|K| |x| + |rhs|) in the infinity norm, plus the
//   smallest normal double for subnormal rows, so the
//   derivative lies on the constraint manifold to that accuracy.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "svi/arbitrage.hpp"
#include "svi/calibration_adjoint.hpp"
#include "svi/fit.hpp"
#include "svi/implied_vol.hpp"

// A failed property aborts, so libFuzzer keeps the input, and says which one failed.
#define AADP_FUZZ_REQUIRE(condition, what)                                                   \
  do {                                                                                       \
    if (!(condition)) {                                                                      \
      std::fprintf(stderr, "invariant failed: %s: %s (%s:%d)\n", what, #condition, __FILE__, \
                   __LINE__);                                                                \
      std::abort();                                                                          \
    }                                                                                        \
  } while (false)

namespace {

using namespace aadp::svi;

constexpr std::size_t kHeaderBytes = 80;
constexpr std::size_t kQuoteBytes = 24;
constexpr std::size_t kMaxQuotes = 12;
constexpr double kResidual = 0x1p-36;

constexpr FitOptions kFitOptions{.grid_m = 3,
                                 .grid_sigma = 3,
                                 .starts = 2,
                                 .max_evaluations_per_start = 40,
                                 .max_inner_steps = 20};

double ReadF64(const std::uint8_t* data, std::size_t offset) {
  double value = 0.0;
  std::memcpy(&value, data + offset, sizeof value);
  return value;
}

bool Positive(double x) { return std::isfinite(x) && x > 0.0; }

bool Named(std::string_view name) { return !name.empty() && name != "unknown"; }

bool AllFinite(std::span<const double> v) {
  return std::ranges::all_of(v, [](double x) { return std::isfinite(x); });
}

bool GoodParameters(const SviParams& p) {
  const std::array<double, 5> t = {p.a, p.b, p.rho, p.m, p.sigma};
  return AllFinite(t) && p.b > 0.0 && std::abs(p.rho) <= 1.0 && p.sigma > 0.0;
}

bool Supported(std::span<const Bound> active, const SviParams& p) {
  if (active.size() > kMaxActiveConstraints) return false;
  return std::ranges::all_of(active, [&](Bound b) {
    return b == Bound::kRhoLower || b == Bound::kRhoUpper || (b == Bound::kSlope && p.rho != 0.0);
  });
}

void CheckError(const FitSlice& slice, const SviParams& p, std::span<const Bound> active,
                double sigma_leg, const AdjointError& e, const AdjointOptions& options) {
  AADP_FUZZ_REQUIRE(e.code <= AdjointErrorCode::kIllConditioned, "known error code");
  AADP_FUZZ_REQUIRE(Named(AdjointErrorName(e.code)), "error has a name");
  const bool good_slice =
      Positive(slice.forward) && Positive(slice.discount) && Positive(slice.expiry);
  AADP_FUZZ_REQUIRE((e.code == AdjointErrorCode::kBadSlice) == !good_slice, "bad slice when it is");
  const bool good_leg = std::isfinite(sigma_leg) && sigma_leg >= 0.0;
  if (e.code == AdjointErrorCode::kBadSigmaLeg) {
    AADP_FUZZ_REQUIRE(!good_leg, "bad sigma_leg is bad");
  }
  if (e.code == AdjointErrorCode::kBadQuote && !slice.quotes.empty()) {
    AADP_FUZZ_REQUIRE(e.quote < slice.quotes.size(), "bad quote inside the slice");
    const FitQuote& q = slice.quotes[e.quote];
    AADP_FUZZ_REQUIRE(
        !Positive(q.strike) || !Positive(q.price) || !Positive(q.noise) || q.noise < sigma_leg,
        "bad quote is bad");
  }
  if (e.code == AdjointErrorCode::kBadParameters) {
    AADP_FUZZ_REQUIRE(!GoodParameters(p), "bad parameters are bad");
  }
  if (e.code == AdjointErrorCode::kNonPositiveVariance) {
    AADP_FUZZ_REQUIRE(e.quote < slice.quotes.size(), "variance quote inside the slice");
  }
  if (e.code == AdjointErrorCode::kUnsupportedBound) {
    AADP_FUZZ_REQUIRE(!Supported(active, p), "refused bound");
  }
  if (e.code == AdjointErrorCode::kIllConditioned) {
    AADP_FUZZ_REQUIRE(!(e.value <= options.max_condition), "ill-conditioned above the limit");
  }
  if (e.code == AdjointErrorCode::kDegenerateMultiplier) {
    AADP_FUZZ_REQUIRE(!active.empty(), "a multiplier needs a constraint");
  }
}

struct Constraint {
  Vector5 gradient{};
  Matrix5 hessian{};
};

// The constraints' formulas, written out again: c = b (1 + |rho|) - L, rho - 1, -1 - rho.
std::vector<Constraint> Model(std::span<const Bound> active, const SviParams& p) {
  std::vector<Constraint> out;
  for (const Bound b : active) {
    Constraint c;
    if (b == Bound::kSlope) {
      const double s = p.rho > 0.0 ? 1.0 : -1.0;
      c.gradient = {0.0, 1.0 + std::abs(p.rho), s * p.b, 0.0, 0.0};
      c.hessian[1][2] = s;
      c.hessian[2][1] = s;
    } else {
      c.gradient[2] = b == Bound::kRhoUpper ? 1.0 : -1.0;
    }
    out.push_back(c);
  }
  return out;
}

// The scaled residual of one solution of the KKT system, normwise: every row of
// K x - rhs within kResidual (|K| |x| + |rhs|) in the infinity norm, the backward error that
// Gaussian elimination with partial pivoting guarantees up to its growth factor. (Row by row
// against that row's own terms is too strict: with b and rho both fixed by two active
// constraints, d b and d rho are rounding noise.)
void CheckSolution(const Matrix5& h_l, std::span<const Constraint> cs, const Vector5& scale,
                   const Vector5& cross, const Vector5& d_theta, const Multipliers& d_mu,
                   const char* what) {
  std::array<double, 5> u{};
  for (std::size_t j = 0; j < 5; ++j) u[j] = d_theta[j] / scale[j];
  double x_norm = 0.0;
  for (const double v : u) x_norm = std::max(x_norm, std::abs(v));
  for (std::size_t k = 0; k < cs.size(); ++k) x_norm = std::max(x_norm, std::abs(d_mu[k]));
  double k_norm = 0.0;
  double rhs_norm = 0.0;
  std::array<double, 5 + kMaxActiveConstraints> residual{};
  for (std::size_t i = 0; i < 5; ++i) {
    double row = 0.0;
    residual[i] = cross[i] * scale[i];
    rhs_norm = std::max(rhs_norm, std::abs(residual[i]));
    for (std::size_t j = 0; j < 5; ++j) {
      const double kij = scale[i] * h_l[i][j] * scale[j];
      residual[i] += kij * u[j];
      row += std::abs(kij);
    }
    for (std::size_t k = 0; k < cs.size(); ++k) {
      const double kik = scale[i] * cs[k].gradient[i];
      residual[i] += kik * d_mu[k];
      row += std::abs(kik);
    }
    k_norm = std::max(k_norm, row);
  }
  for (std::size_t k = 0; k < cs.size(); ++k) {
    double row = 0.0;
    for (std::size_t j = 0; j < 5; ++j) {
      residual[5 + k] += cs[k].gradient[j] * scale[j] * u[j];
      row += std::abs(cs[k].gradient[j] * scale[j]);
    }
    k_norm = std::max(k_norm, row);
  }
  // Plus the smallest normal double: with a subnormal sigma_leg its row is subnormal, where
  // rounding is absolute.
  const double bound =
      (kResidual * ((k_norm * x_norm) + rhs_norm)) + std::numeric_limits<double>::min();
  for (std::size_t i = 0; i < 5; ++i) AADP_FUZZ_REQUIRE(std::abs(residual[i]) <= bound, what);
  for (std::size_t k = 0; k < cs.size(); ++k) {
    AADP_FUZZ_REQUIRE(std::abs(residual[5 + k]) <= bound, "on the constraint manifold");
  }
}

void CheckSensitivity(const ParameterSensitivity& s, const Matrix5& hessian,
                      std::span<const Constraint> cs, const Multipliers& mu, const Vector5& scale,
                      std::span<const Vector5> quote_cross,
                      std::span<const Vector5, kMarketInputs> market_cross,
                      const AdjointOptions& options) {
  AADP_FUZZ_REQUIRE(s.quote.size() == quote_cross.size(), "one derivative per quote");
  AADP_FUZZ_REQUIRE(s.quote_multiplier.size() == quote_cross.size(),
                    "one multiplier row per quote");
  AADP_FUZZ_REQUIRE(s.condition >= 1.0 - 1e-12 && s.condition <= options.max_condition,
                    "condition in range");
  AADP_FUZZ_REQUIRE(std::isfinite(s.manifold_residual), "manifold residual finite");
  Matrix5 h_l = hessian;
  for (std::size_t k = 0; k < cs.size(); ++k) {
    for (std::size_t i = 0; i < 5; ++i) {
      for (std::size_t j = 0; j < 5; ++j) h_l[i][j] += mu[k] * cs[k].hessian[i][j];
    }
  }
  for (std::size_t i = 0; i < s.quote.size(); ++i) {
    AADP_FUZZ_REQUIRE(AllFinite(s.quote[i]) && AllFinite(s.quote_multiplier[i]),
                      "finite derivative");
    CheckSolution(h_l, cs, scale, quote_cross[i], s.quote[i], s.quote_multiplier[i],
                  "quote KKT residual");
  }
  for (std::size_t p = 0; p < kMarketInputs; ++p) {
    AADP_FUZZ_REQUIRE(AllFinite(s.market[p]) && AllFinite(s.market_multiplier[p]),
                      "finite derivative");
    CheckSolution(h_l, cs, scale, market_cross[p], s.market[p], s.market_multiplier[p],
                  "market KKT residual");
  }
}

void CheckAdjoint(const FitSlice& slice, const SviParams& p, std::span<const Bound> active,
                  const CalibrationAdjoint& a, const AdjointOptions& options) {
  const ObjectiveDerivatives& d = a.derivatives;
  AADP_FUZZ_REQUIRE(std::isfinite(d.objective) && d.objective >= 0.0, "Phi finite, non-negative");
  AADP_FUZZ_REQUIRE(d.quote_cross.size() == slice.quotes.size(), "one cross row per quote");
  AADP_FUZZ_REQUIRE(a.constraints.size() == active.size(), "one constraint per active bound");
  Multipliers mu{};
  for (std::size_t k = 0; k < a.constraints.size(); ++k) {
    const ActiveConstraint& c = a.constraints[k];
    AADP_FUZZ_REQUIRE(c.bound == active[k], "constraints in order");
    AADP_FUZZ_REQUIRE(c.multiplier > 0.0 && std::isfinite(c.multiplier), "mu positive");
    AADP_FUZZ_REQUIRE(c.margin >= options.min_multiplier_margin, "mu clear of its uncertainty");
    mu[k] = c.multiplier;
  }
  AADP_FUZZ_REQUIRE(std::isfinite(a.stationarity) && std::isfinite(a.gradient_norm),
                    "finite norms");
  AADP_FUZZ_REQUIRE(AllFinite(a.newton_step), "finite Newton step");
  Vector5 scale{};
  for (std::size_t j = 0; j < 5; ++j) {
    AADP_FUZZ_REQUIRE(d.gauss_newton[j][j] > 0.0, "positive Gauss-Newton diagonal");
    scale[j] = 1.0 / std::sqrt(d.gauss_newton[j][j]);
  }
  const std::vector<Constraint> cs = Model(active, p);
  CheckSensitivity(a.exact, d.hessian, cs, mu, scale, d.quote_cross, d.market_cross, options);
  if (a.gauss_newton) {
    CheckSensitivity(*a.gauss_newton, d.gauss_newton, cs, mu, scale, d.quote_cross,
                     d.market_cross_gauss_newton, options);
  } else {
    AADP_FUZZ_REQUIRE(Named(AdjointErrorName(a.gauss_newton.error().code)), "GN error named");
  }
}

void Run(const FitSlice& slice, const SviParams& p, std::span<const Bound> active, double sigma_leg,
         const AdjointOptions& options) {
  const auto a = CalibrationAdjointAt(slice, p, active, sigma_leg, options);
  if (a) {
    CheckAdjoint(slice, p, active, *a, options);
  } else {
    CheckError(slice, p, active, sigma_leg, a.error(), options);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < kHeaderBytes) return 0;
  const std::size_t wanted = data[0] % (kMaxQuotes + 1);
  const std::size_t n = std::min(wanted, (size - kHeaderBytes) / kQuoteBytes);
  std::uint16_t puts = 0;
  std::memcpy(&puts, data + 2, sizeof puts);
  FitSlice slice{.forward = ReadF64(data, 8),
                 .discount = ReadF64(data, 16),
                 .expiry = ReadF64(data, 24),
                 .quotes = {}};
  const SviParams p{.a = ReadF64(data, 32),
                    .b = ReadF64(data, 40),
                    .rho = ReadF64(data, 48),
                    .m = ReadF64(data, 56),
                    .sigma = ReadF64(data, 64)};
  const double sigma_leg = ReadF64(data, 72);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t at = kHeaderBytes + (i * kQuoteBytes);
    slice.quotes.push_back({.strike = ReadF64(data, at),
                            .price = ReadF64(data, at + 8),
                            .type = ((static_cast<unsigned>(puts) >> i) & 1U) != 0U
                                        ? OptionType::kPut
                                        : OptionType::kCall,
                            .noise = ReadF64(data, at + 16)});
  }
  std::vector<Bound> active;
  if ((data[1] & 1U) != 0) active.push_back(Bound::kSlope);
  if ((data[1] & 2U) != 0) active.push_back(Bound::kRhoLower);
  if ((data[1] & 4U) != 0) active.push_back(Bound::kRhoUpper);
  if ((data[1] & 8U) != 0) active.push_back(Bound::kMLower);
  const AdjointOptions options{};
  Run(slice, p, active, sigma_leg, options);
  if ((data[4] & 1U) != 0) {
    // At a fit, with its own active set, through the same entry point.
    const auto fit = FitRawSvi(slice, kFitOptions);
    if (fit) {
      const std::vector<Bound> fitted = fit->ActiveBounds();
      const auto a = CalibrationAdjointOf(slice, *fit, sigma_leg, options);
      if (a) {
        CheckAdjoint(slice, fit->params, fitted, *a, options);
      } else {
        CheckError(slice, fit->params, fitted, sigma_leg, a.error(), options);
      }
    }
  }
  return 0;
}
