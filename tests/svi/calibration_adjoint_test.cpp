// The calibration adjoint (src/svi/calibration_adjoint.hpp) and the pieces of the bump check
// (src/svi/chain_adjoint.hpp), on hand-made slices only, never chain data. Every tolerance here
// is stated in docs/NOTES.md, "Stage 7b". The tests ran while the code was written, on these
// synthetic slices only, and no tolerance was changed after a test of it failed.
//
// The references are independent of the second-order tape: a closed-form Jacobian of Black-76 in
// the parameters for the exact fit, and nested forward-mode Duals (ad/dual.hpp) of the objective
// written out here for the rest.
#include "svi/calibration_adjoint.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ad/dual.hpp"
#include "data/calibration_set.hpp"
#include "data/forwards.hpp"
#include "data/taifex_options.hpp"
#include "rng/normal.hpp"
#include "rng/philox.hpp"
#include "svi/arbitrage.hpp"
#include "svi/chain_adjoint.hpp"
#include "svi/fit.hpp"
#include "svi/implied_vol.hpp"
#include "svi/raw_svi.hpp"

namespace aadp::svi {
namespace {

constexpr double kForward = 22800.0;
constexpr double kSigmaLeg = 2.0;
// A development seed, never 1729 or 271828.
constexpr std::uint64_t kNoiseSeed = 20261124;

// Agreement of two computations of a derivative of theta, in the KKT's scaled units
// u_j = theta_j sqrt(GN_jj): within kAgreement kappa of the reference's largest scaled entry,
// kappa the scaled KKT matrix's condition number.
constexpr double kAgreement = 0x1p-42;
// Hessian entries of two exact computations: within kHessianAgreement sqrt(GN_jj GN_kk).
constexpr double kHessianAgreement = 0x1p-36;

double Tick(double price) {
  if (price < 10.0) return 0.1;
  if (price < 50.0) return 0.5;
  if (price < 500.0) return 1.0;
  if (price < 1000.0) return 5.0;
  return 10.0;
}

struct Spec {
  double expiry = 0.0;
  double discount = 1.0;
  SviParams params{};
  double first_strike = 0.0;
  double last_strike = 0.0;
};

const Spec kA{.expiry = 5.0 / 365.0,
              .discount = 0.9999,
              .params = {.a = 1e-4, .b = 0.012, .rho = -0.45, .m = 0.002, .sigma = 0.025},
              .first_strike = 19700.0,
              .last_strike = 24100.0};
const Spec kB{.expiry = 12.0 / 365.0,
              .discount = 0.9995,
              .params = {.a = 2e-4, .b = 0.02, .rho = -0.35, .m = 0.005, .sigma = 0.04},
              .first_strike = 19500.0,
              .last_strike = 24500.0};
const Spec kC{.expiry = 33.0 / 365.0,
              .discount = 0.998,
              .params = {.a = 5e-4, .b = 0.04, .rho = -0.5, .m = 0.01, .sigma = 0.06},
              .first_strike = 18000.0,
              .last_strike = 25000.0};

OptionType OtmType(double strike) {
  return strike < kForward ? OptionType::kPut : OptionType::kCall;
}

double Price(const Spec& s, double strike) {
  const double w = TotalVariance(s.params, std::log(strike / kForward));
  return Black76Price(kForward, s.discount, strike, s.expiry, std::sqrt(w / s.expiry),
                      OtmType(strike));
}

double Noise(double price) {
  return std::sqrt((kSigmaLeg * kSigmaLeg) + (Tick(price) * Tick(price) / 12.0));
}

// Out-of-the-money quotes; noisy adds noise_i z_i, z_i from Philox on (kNoiseSeed, index, salt).
FitSlice MakeSlice(const Spec& s, std::span<const double> strikes, bool noisy, std::uint32_t salt) {
  FitSlice slice{.forward = kForward, .discount = s.discount, .expiry = s.expiry, .quotes = {}};
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    double price = Price(s, strikes[i]);
    if (noisy) price += Noise(price) * inverse_normal(PathUniform(kNoiseSeed, i, salt));
    if (!(price >= 1.0)) continue;
    slice.quotes.push_back(
        {.strike = strikes[i], .price = price, .type = OtmType(strikes[i]), .noise = Noise(price)});
  }
  return slice;
}

std::vector<double> GridStrikes(const Spec& s) {
  std::vector<double> out;
  for (int i = 0;; ++i) {
    const double k = s.first_strike + (100.0 * i);
    if (k > s.last_strike) break;
    if (k != kForward) out.push_back(k);
  }
  return out;
}

Vector5 AsVector(const SviParams& p) { return {p.a, p.b, p.rho, p.m, p.sigma}; }

// Gaussian elimination with partial pivoting on a small dense system; x = A^-1 b.
std::vector<double> Solve(std::vector<std::vector<double>> a, std::vector<double> b) {
  const std::size_t n = b.size();
  for (std::size_t c = 0; c < n; ++c) {
    std::size_t p = c;
    for (std::size_t r = c + 1; r < n; ++r) {
      if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
    }
    std::swap(a[c], a[p]);
    std::swap(b[c], b[p]);
    for (std::size_t r = c + 1; r < n; ++r) {
      const double f = a[r][c] / a[c][c];
      for (std::size_t j = c; j < n; ++j) a[r][j] -= f * a[c][j];
      b[r] -= f * b[c];
    }
  }
  std::vector<double> x(n);
  for (std::size_t c = n; c-- > 0;) {
    double v = b[c];
    for (std::size_t j = c + 1; j < n; ++j) v -= a[c][j] * x[j];
    x[c] = v / a[c][c];
  }
  return x;
}

// max_j |got_j - want_j| s_j over max_j |want_j| s_j, s_j = sqrt(GN_jj).
double ScaledError(const Vector5& got, std::span<const double> want, const Matrix5& gn) {
  double error = 0.0;
  double size = 0.0;
  for (std::size_t j = 0; j < 5; ++j) {
    const double s = std::sqrt(gn[j][j]);
    error = std::max(error, std::abs(got[j] - want[j]) * s);
    size = std::max(size, std::abs(want[j]) * s);
  }
  return size > 0.0 ? error / size : error;
}

// ---- The objective in forward mode, written out independently of the tape. ----

template <std::size_t N>
using First = ad::Dual<double, N>;
template <std::size_t N>
using Nested = ad::Dual<First<N>, N>;

template <std::size_t N>
Nested<N> SeedNested(double v, std::size_t lane) {
  Nested<N> x(v);
  x.value.tangent[lane] = 1.0;
  x.tangent[lane].value = 1.0;
  return x;
}

template <std::size_t N>
First<N> SeedFirst(double v, std::size_t lane) {
  First<N> x(v);
  x.tangent[lane] = 1.0;
  return x;
}

template <class T>
struct Inputs {
  std::array<T, 5> theta;
  T forward;
  T discount;
  T leg;
};

// r_i, with noise_i = sqrt(s^2 + tau_i^2) and tau_i^2 = noise_i^2 - kSigmaLeg^2 held.
template <class T>
T ResidualOf(const FitQuote& q, const Inputs<T>& in) {
  const T k = ad::Log(q.strike / in.forward);
  const T x = k - in.theta[3];
  const T w = in.theta[0] +
              (in.theta[1] * ((in.theta[2] * x) + ad::Sqrt((x * x) + (in.theta[4] * in.theta[4]))));
  const T root = ad::Sqrt(w);
  const T d1 = ((0.5 * w) - k) / root;
  const T d2 = d1 - root;
  const T model =
      q.type == OptionType::kCall
          ? in.discount * ((in.forward * ad::NormalCdf(d1)) - (q.strike * ad::NormalCdf(d2)))
          : in.discount * ((q.strike * ad::NormalCdf(-d2)) - (in.forward * ad::NormalCdf(-d1)));
  const double tau2 = (q.noise * q.noise) - (kSigmaLeg * kSigmaLeg);
  return (model - q.price) / ad::Sqrt((in.leg * in.leg) + tau2);
}

template <class T>
T PhiOf(const FitSlice& slice, const Inputs<T>& in) {
  T phi(0.0);
  for (const FitQuote& q : slice.quotes) {
    const T r = ResidualOf(q, in);
    phi += r * r;
  }
  return phi;
}

// Lanes 0-4 theta, 5 F, 6 D, 7 s.
template <class T, class Seed>
Inputs<T> RawInputs(const FitSlice& slice, const SviParams& p, Seed seed) {
  return {.theta = {seed(p.a, 0), seed(p.b, 1), seed(p.rho, 2), seed(p.m, 3), seed(p.sigma, 4)},
          .forward = seed(slice.forward, 5),
          .discount = seed(slice.discount, 6),
          .leg = seed(kSigmaLeg, 7)};
}

// The reduced problem on the slope constraint with rho < 0: phi = (a, rho, m, sigma), b = L / (1
// - rho). Lanes 0-3 phi, 4 F, 5 D, 6 s.
template <class T, class Seed>
Inputs<T> ReducedInputs(const FitSlice& slice, const std::array<double, 4>& phi, double bound,
                        Seed seed) {
  const T rho = seed(phi[1], 1);
  return {.theta = {seed(phi[0], 0), bound / (1.0 - rho), rho, seed(phi[2], 2), seed(phi[3], 3)},
          .forward = seed(slice.forward, 4),
          .discount = seed(slice.discount, 5),
          .leg = seed(kSigmaLeg, 6)};
}

// ---- The exact fit: five quotes, five parameters, so the derivative is G^-1. ----

// G_ij = d B_i / d theta_j and d B_i / d(F, D), in closed form with std::erfc.
struct ClosedForm {
  std::vector<std::vector<double>> g;
  std::vector<double> d_forward;
  std::vector<double> d_discount;
};

ClosedForm Jacobian(const FitSlice& slice, const SviParams& p) {
  ClosedForm out;
  const double f = slice.forward;
  const double disc = slice.discount;
  for (const FitQuote& q : slice.quotes) {
    const double k = std::log(q.strike / f);
    const double x = k - p.m;
    const double root_x = std::sqrt((x * x) + (p.sigma * p.sigma));
    const double w = p.a + (p.b * ((p.rho * x) + root_x));
    const double sw = std::sqrt(w);
    const double d1 = ((0.5 * w) - k) / sw;
    const double phi = std::exp(-0.5 * d1 * d1) / std::sqrt(2.0 * std::numbers::pi);
    const double vega_w = disc * f * phi / (2.0 * sw);
    out.g.push_back({vega_w, vega_w * ((p.rho * x) + root_x), vega_w * p.b * x,
                     -vega_w * p.b * (p.rho + (x / root_x)), vega_w * p.b * p.sigma / root_x});
    const double n_d1 = 0.5 * std::erfc(-d1 / std::numbers::sqrt2);
    const double delta = q.type == OptionType::kCall ? disc * n_d1 : -disc * (1.0 - n_d1);
    const double dw_dk = p.b * (p.rho + (x / root_x));
    out.d_forward.push_back(delta - (vega_w * dw_dk / f));
    out.d_discount.push_back(q.price / disc);
  }
  return out;
}

TEST(CalibrationAdjoint, ExactFitEqualsTheDirectSolve) {
  const std::array<double, 5> strikes = {21800.0, 22400.0, 22700.0, 23000.0, 23600.0};
  const FitSlice slice = MakeSlice(kB, strikes, false, 0);
  ASSERT_EQ(slice.quotes.size(), 5U);
  const auto adjoint = CalibrationAdjointAt(slice, kB.params, {}, 0.0);
  ASSERT_TRUE(adjoint.has_value()) << AdjointErrorName(adjoint.error().code);
  ASSERT_TRUE(adjoint->gauss_newton.has_value());
  EXPECT_TRUE(adjoint->constraints.empty());
  const Matrix5& gn = adjoint->derivatives.gauss_newton;
  const double kappa = adjoint->exact.condition;
  const double tolerance = kAgreement * kappa;
  const ClosedForm cf = Jacobian(slice, kB.params);
  for (std::size_t i = 0; i < 5; ++i) {
    std::vector<double> e(5, 0.0);
    e[i] = 1.0;
    const std::vector<double> want = Solve(cf.g, e);
    EXPECT_LE(ScaledError(adjoint->exact.quote[i], want, gn), tolerance) << "quote " << i;
    EXPECT_LE(ScaledError(adjoint->gauss_newton->quote[i], want, gn), tolerance) << "quote " << i;
  }
  std::vector<double> minus_f(5);
  std::vector<double> minus_d(5);
  for (std::size_t i = 0; i < 5; ++i) {
    minus_f[i] = -cf.d_forward[i];
    minus_d[i] = -cf.d_discount[i];
  }
  const std::vector<double> want_f = Solve(cf.g, minus_f);
  const std::vector<double> want_d = Solve(cf.g, minus_d);
  EXPECT_LE(ScaledError(adjoint->exact.market[0], want_f, gn), tolerance);
  EXPECT_LE(ScaledError(adjoint->exact.market[1], want_d, gn), tolerance);
  // sigma_leg = 0: the noise does not move, so neither does theta.
  for (const double v : adjoint->exact.market[2]) EXPECT_EQ(v, 0.0);
  std::printf("exact fit: kappa %.4e, tolerance %.3e\n", kappa, tolerance);
}

// ---- The tape's derivatives of Phi against nested forward mode. ----

TEST(CalibrationAdjoint, DerivativesMatchForwardMode) {
  for (const Spec* s : {&kA, &kB, &kC}) {
    const std::vector<double> strikes = GridStrikes(*s);
    const FitSlice slice = MakeSlice(*s, strikes, true, 1);
    const auto fit = FitRawSvi(slice);
    ASSERT_TRUE(fit.has_value());
    const auto d = DifferentiateObjective(slice, fit->params, kSigmaLeg);
    ASSERT_TRUE(d.has_value()) << AdjointErrorName(d.error().code);

    const Inputs<Nested<8>> in = RawInputs<Nested<8>>(
        slice, fit->params, [](double v, std::size_t l) { return SeedNested<8>(v, l); });
    const Nested<8> phi = PhiOf(slice, in);
    std::array<std::array<double, 8>, 8> gn{};
    std::vector<First<8>> residuals;
    for (const FitQuote& q : slice.quotes) {
      const First<8> r = ResidualOf(
          q, RawInputs<First<8>>(slice, fit->params,
                                 [](double v, std::size_t l) { return SeedFirst<8>(v, l); }));
      residuals.push_back(r);
      for (std::size_t j = 0; j < 8; ++j) {
        for (std::size_t k = 0; k < 8; ++k) gn[j][k] += 2.0 * r.tangent[j] * r.tangent[k];
      }
    }
    EXPECT_NEAR(d->objective, phi.value.value, 1e-12 * phi.value.value);
    for (std::size_t j = 0; j < 5; ++j) {
      const double sj = std::sqrt(gn[j][j]);
      EXPECT_NEAR(d->gradient[j], phi.value.tangent[j], kHessianAgreement * sj) << j;
      for (std::size_t k = 0; k < 5; ++k) {
        const double scale = kHessianAgreement * sj * std::sqrt(gn[k][k]);
        EXPECT_NEAR(d->hessian[j][k], phi.tangent[j].tangent[k], scale) << j << "," << k;
        EXPECT_NEAR(d->gauss_newton[j][k], gn[j][k], scale) << j << "," << k;
      }
      for (std::size_t p = 0; p < 3; ++p) {
        const double scale = kHessianAgreement * sj * std::sqrt(gn[5 + p][5 + p]);
        EXPECT_NEAR(d->market_cross[p][j], phi.tangent[5 + p].tangent[j], scale) << p << "," << j;
        EXPECT_NEAR(d->market_cross_gauss_newton[p][j], gn[5 + p][j], scale) << p << "," << j;
      }
      for (std::size_t i = 0; i < slice.quotes.size(); ++i) {
        // r_i is linear in V_i with slope -1 / noise_i.
        const double want = -2.0 * residuals[i].tangent[j] / slice.quotes[i].noise;
        EXPECT_NEAR(d->quote_cross[i][j], want,
                    kHessianAgreement * sj * 2.0 / slice.quotes[i].noise)
            << i << "," << j;
      }
    }
    EXPECT_LE(d->hessian_asymmetry, kHessianAgreement);
  }
}

// ---- The slope constraint: the KKT system against the reduced problem. ----

struct Reduced {
  std::array<double, 4> phi{};
  Nested<7> objective;
};

Reduced EvaluateReduced(const FitSlice& slice, const std::array<double, 4>& phi, double bound) {
  return {.phi = phi,
          .objective =
              PhiOf(slice, ReducedInputs<Nested<7>>(slice, phi, bound, [](double v, std::size_t l) {
                      return SeedNested<7>(v, l);
                    }))};
}

TEST(CalibrationAdjoint, SlopeConstraintMatchesTheReducedProblem) {
  const std::vector<double> strikes = GridStrikes(kC);
  const FitSlice slice = MakeSlice(kC, strikes, true, 2);
  FitOptions options;
  options.slope_bound = 0.8 * kC.params.b * (1.0 + std::abs(kC.params.rho));
  const auto fit = FitRawSvi(slice, options);
  ASSERT_TRUE(fit.has_value());
  ASSERT_TRUE(fit->converged);
  const std::vector<Bound> active = fit->ActiveBounds();
  ASSERT_EQ(active.size(), 1U);
  ASSERT_EQ(active[0], Bound::kSlope);
  ASSERT_LT(fit->params.rho, 0.0);
  const double bound = options.slope_bound;

  // Newton on the reduced problem from the fitted point, to its stationary point.
  std::array<double, 4> phi = {fit->params.a, fit->params.rho, fit->params.m, fit->params.sigma};
  Reduced r = EvaluateReduced(slice, phi, bound);
  for (int iteration = 0; iteration < 8; ++iteration) {
    std::vector<std::vector<double>> h(4, std::vector<double>(4));
    std::vector<double> minus_g(4);
    for (std::size_t j = 0; j < 4; ++j) {
      minus_g[j] = -r.objective.value.tangent[j];
      for (std::size_t k = 0; k < 4; ++k) h[j][k] = r.objective.tangent[j].tangent[k];
    }
    const std::vector<double> step = Solve(h, minus_g);
    for (std::size_t j = 0; j < 4; ++j) phi[j] += step[j];
    r = EvaluateReduced(slice, phi, bound);
  }
  const SviParams polished{
      .a = phi[0], .b = bound / (1.0 - phi[1]), .rho = phi[1], .m = phi[2], .sigma = phi[3]};

  const auto adjoint = CalibrationAdjointAt(slice, polished, active, kSigmaLeg);
  ASSERT_TRUE(adjoint.has_value()) << AdjointErrorName(adjoint.error().code);
  ASSERT_EQ(adjoint->constraints.size(), 1U);
  const Matrix5& gn = adjoint->derivatives.gauss_newton;
  const double kappa = adjoint->exact.condition;
  const double tolerance = kAgreement * kappa;

  // The reduced IFT: d phi / d p = -H_r^-1 d2 Phi_r / d phi dp, then theta(phi)'s Jacobian.
  std::vector<std::vector<double>> h(4, std::vector<double>(4));
  for (std::size_t j = 0; j < 4; ++j) {
    for (std::size_t k = 0; k < 4; ++k) h[j][k] = r.objective.tangent[j].tangent[k];
  }
  const double db_drho = bound / ((1.0 - phi[1]) * (1.0 - phi[1]));
  const auto to_theta = [&](const std::vector<double>& d_phi) {
    return std::vector<double>{d_phi[0], db_drho * d_phi[1], d_phi[1], d_phi[2], d_phi[3]};
  };
  for (std::size_t p = 0; p < 3; ++p) {
    std::vector<double> rhs(4);
    for (std::size_t j = 0; j < 4; ++j) rhs[j] = -r.objective.tangent[4 + p].tangent[j];
    EXPECT_LE(ScaledError(adjoint->exact.market[p], to_theta(Solve(h, rhs)), gn), tolerance)
        << "market " << p;
  }
  double gauss_newton_gap = 0.0;
  for (std::size_t i = 0; i < slice.quotes.size(); ++i) {
    const First<7> ri = ResidualOf(
        slice.quotes[i], ReducedInputs<First<7>>(slice, phi, bound, [](double v, std::size_t l) {
          return SeedFirst<7>(v, l);
        }));
    std::vector<double> rhs(4);
    for (std::size_t j = 0; j < 4; ++j) rhs[j] = 2.0 * ri.tangent[j] / slice.quotes[i].noise;
    const std::vector<double> want = to_theta(Solve(h, rhs));
    EXPECT_LE(ScaledError(adjoint->exact.quote[i], want, gn), tolerance) << "quote " << i;
    // Gauss-Newton with the constraint's curvature can be indefinite on the manifold, a refusal.
    gauss_newton_gap =
        adjoint->gauss_newton
            ? std::max(gauss_newton_gap, ScaledError(adjoint->gauss_newton->quote[i], want, gn))
            : std::numeric_limits<double>::infinity();
    // On the manifold: (1 + |rho|) d b + b sign(rho) d rho = 0.
    const Vector5& d = adjoint->exact.quote[i];
    const double along = ((1.0 - polished.rho) * d[1]) - (polished.b * d[2]);
    EXPECT_LE(std::abs(along),
              0x1p-40 * (std::abs((1.0 - polished.rho) * d[1]) + std::abs(polished.b * d[2])))
        << "quote " << i;
  }
  // Gauss-Newton drops the residual curvature, which is not small on a noisy slice: it refuses
  // or disagrees.
  EXPECT_GT(gauss_newton_gap, 1e3 * tolerance);

  // The multiplier: at the KKT point d Phi / d b + mu (1 + |rho|) = 0.
  const First<5> raw_phi = PhiOf(
      slice, Inputs<First<5>>{.theta = {SeedFirst<5>(polished.a, 0), SeedFirst<5>(polished.b, 1),
                                        SeedFirst<5>(polished.rho, 2), SeedFirst<5>(polished.m, 3),
                                        SeedFirst<5>(polished.sigma, 4)},
                              .forward = First<5>(slice.forward),
                              .discount = First<5>(slice.discount),
                              .leg = First<5>(kSigmaLeg)});
  const double mu = -raw_phi.tangent[1] / (1.0 - polished.rho);
  EXPECT_GT(mu, 0.0);
  EXPECT_NEAR(adjoint->constraints[0].multiplier, mu, tolerance * mu);
  EXPECT_GE(adjoint->constraints[0].margin, AdjointOptions{}.min_multiplier_margin);
  EXPECT_LE(adjoint->exact.manifold_residual, 0x1p-40);
  std::printf(
      "slope constraint: kappa %.4e, tolerance %.3e, mu %.6e, margin %.3e, GN %s, gap %.3e\n",
      kappa, tolerance, mu, adjoint->constraints[0].margin,
      adjoint->gauss_newton ? "solved" : AdjointErrorName(adjoint->gauss_newton.error().code),
      gauss_newton_gap);
}

// ---- Error values. ----

TEST(CalibrationAdjoint, RefusalsAreErrorValues) {
  const std::vector<double> strikes = GridStrikes(kB);
  const FitSlice slice = MakeSlice(kB, strikes, false, 0);
  const auto code = [](const auto& result) -> std::optional<AdjointErrorCode> {
    if (result.has_value()) return std::nullopt;
    return result.error().code;
  };
  const std::array<Bound, 1> m_lower = {Bound::kMLower};
  EXPECT_EQ(code(CalibrationAdjointAt(slice, kB.params, m_lower)),
            AdjointErrorCode::kUnsupportedBound);
  SviParams flat = kB.params;
  flat.rho = 0.0;
  const std::array<Bound, 1> slope = {Bound::kSlope};
  EXPECT_EQ(code(CalibrationAdjointAt(slice, flat, slope)), AdjointErrorCode::kUnsupportedBound);
  // At the exact fit the gradient is zero, so a slope multiplier would be zero.
  EXPECT_EQ(code(CalibrationAdjointAt(slice, kB.params, slope)),
            AdjointErrorCode::kDegenerateMultiplier);
  EXPECT_EQ(
      code(CalibrationAdjointAt(slice, kB.params, {}, 0.0, AdjointOptions{.max_condition = 1.0})),
      AdjointErrorCode::kIllConditioned);
  // Two quotes, five parameters: the Hessian at an exact fit has rank 2.
  FitSlice two = slice;
  two.quotes.resize(2);
  EXPECT_EQ(code(CalibrationAdjointAt(two, kB.params, {})), AdjointErrorCode::kNotMinimum);
  EXPECT_EQ(code(CalibrationAdjointAt(slice, kB.params, {}, -1.0)), AdjointErrorCode::kBadSigmaLeg);
  EXPECT_EQ(code(CalibrationAdjointAt(slice, kB.params, {}, 1e6)), AdjointErrorCode::kBadQuote);
  SviParams bad = kB.params;
  bad.b = 0.0;
  EXPECT_EQ(code(CalibrationAdjointAt(slice, bad, {})), AdjointErrorCode::kBadParameters);
  bad = kB.params;
  bad.a = -1.0;
  EXPECT_EQ(code(CalibrationAdjointAt(slice, bad, {})), AdjointErrorCode::kNonPositiveVariance);
  FitSlice no_forward = slice;
  no_forward.forward = 0.0;
  EXPECT_EQ(code(CalibrationAdjointAt(no_forward, kB.params, {})), AdjointErrorCode::kBadSlice);
}

// ---- The parity regression's derivative against central differences. ----

TEST(ChainAdjoint, ParityJacobianMatchesDifferences) {
  std::vector<ParityPoint> points;
  for (std::size_t j = 0; j < 17; ++j) {
    const double strike = 22000.0 + (100.0 * static_cast<double>(j));
    const double e = 2.0 * inverse_normal(PathUniform(kNoiseSeed, j, 3));
    points.push_back({.strike = strike, .call_minus_put = (0.9995 * (kForward - strike)) + e});
  }
  const auto jacobian = DifferentiateParity(points);
  ASSERT_TRUE(jacobian.has_value());
  const auto base = fit_parity(points);
  ASSERT_TRUE(base.has_value());
  const std::array<double, 3> value = {base->forward, base->discount, base->sigma_leg};
  for (std::size_t j = 0; j < points.size(); ++j) {
    std::array<std::array<double, 2>, 3> difference{};
    for (std::size_t level = 0; level < 2; ++level) {
      const double h = std::ldexp(0.01, -static_cast<int>(level));
      std::vector<ParityPoint> up = points;
      std::vector<ParityPoint> down = points;
      up[j].call_minus_put += h;
      down[j].call_minus_put -= h;
      const auto u = fit_parity(up);
      const auto d = fit_parity(down);
      ASSERT_TRUE(u.has_value() && d.has_value());
      difference[0][level] = (u->forward - d->forward) / (2.0 * h);
      difference[1][level] = (u->discount - d->discount) / (2.0 * h);
      difference[2][level] = (u->sigma_leg - d->sigma_leg) / (2.0 * h);
    }
    for (std::size_t p = 0; p < 3; ++p) {
      // Truncation from the two levels, and the outputs' rounding over the smaller h.
      const double tolerance = (4.0 * std::abs(difference[p][0] - difference[p][1])) +
                               (0x1p-44 * std::abs(value[p]) / 0.005);
      EXPECT_NEAR(jacobian->d_market[j][p], difference[p][1], tolerance) << j << "," << p;
    }
  }
}

// A call's settlement is +1 times its parity point's C - P, a put's -1; no point, no leg.
TEST(ChainAdjoint, QuoteLegsFollowTheRight) {
  CalibrationSlice rows{.expiry = "202411", .forward = kForward, .rows = {}};
  for (const auto& [strike, right] : {std::pair{22700.0, taifex::OptionRight::kPut},
                                      std::pair{22900.0, taifex::OptionRight::kCall},
                                      std::pair{23500.0, taifex::OptionRight::kCall}}) {
    taifex::OptionRow row;
    row.strike = strike;
    row.right = right;
    rows.rows.push_back(row);
  }
  const std::vector<ParityPoint> points = {{.strike = 22700.0, .call_minus_put = 99.0},
                                           {.strike = 22900.0, .call_minus_put = -101.0}};
  const std::vector<QuoteLeg> legs = QuoteLegs(rows, points);
  ASSERT_EQ(legs.size(), 3U);
  EXPECT_TRUE(legs[0].present);
  EXPECT_EQ(legs[0].point, 0U);
  EXPECT_EQ(legs[0].sign, -1.0);
  EXPECT_TRUE(legs[1].present);
  EXPECT_EQ(legs[1].point, 1U);
  EXPECT_EQ(legs[1].sign, 1.0);
  EXPECT_FALSE(legs[2].present);
}

// ---- The halving rule. ----

TEST(ChainAdjoint, HalvingRule) {
  std::array<double, 6> h{};
  for (std::size_t l = 0; l < 6; ++l) h[l] = std::ldexp(1.0, -static_cast<int>(l));
  const std::array<bool, 6> all = {true, true, true, true, true, true};
  // An h^2 error: the finest levels are within their truncation of the limit.
  std::array<double, 6> quadratic{};
  for (std::size_t l = 0; l < 6; ++l) quadratic[l] = 1.0 + (0.1 * h[l] * h[l]);
  const HalvingJudgement good = JudgeByHalving(h, quadratic, all, 0.0, 1.0, 0.0);
  EXPECT_TRUE(good.resolved);
  EXPECT_TRUE(good.pass);
  EXPECT_EQ(good.level, 4U);
  // A reference off by much more than the truncation fails.
  EXPECT_FALSE(JudgeByHalving(h, quadratic, all, 0.0, 1.01, 0.0).pass);
  // The refit error grows as 1 / h, so the rule stops halving before it dominates.
  const HalvingJudgement noisy = JudgeByHalving(h, quadratic, all, 1e-3, 1.0, 0.0);
  EXPECT_TRUE(noisy.pass);
  EXPECT_LT(noisy.level, 4U);
  EXPECT_DOUBLE_EQ(noisy.refit, 1e-3 / h[noisy.level]);
  // A sequence that settles only at small h is judged at a fine level, not by a coarse window.
  // (Its first two levels agree closely, so a rule judging by neighbours alone would pick l = 0.)
  const std::array<double, 6> late = {0.54, 0.5401, 0.68, 0.876, 0.9997, 1.0008};
  const HalvingJudgement settled = JudgeByHalving(h, late, all, 0.0, 1.0, 0.0);
  EXPECT_TRUE(settled.pass);
  EXPECT_EQ(settled.level, 4U);
  // No usable level with a finer usable level: unresolved, which counts as a failure.
  const std::array<bool, 6> one = {false, false, true, false, false, false};
  const HalvingJudgement unresolved = JudgeByHalving(h, quadratic, one, 0.0, 1.0, 0.0);
  EXPECT_FALSE(unresolved.resolved);
  EXPECT_FALSE(unresolved.pass);
  // The floor is added to the tolerance.
  const std::array<double, 6> constant = {2.0, 2.0, 2.0, 2.0, 2.0, 2.0};
  EXPECT_FALSE(JudgeByHalving(h, constant, all, 0.0, 2.5, 0.4).pass);
  EXPECT_TRUE(JudgeByHalving(h, constant, all, 0.0, 2.5, 0.5).pass);
}

// ---- Bump and refit on a constrained slice: the check the report runs, in small. ----

constexpr std::size_t kLevels = 6;

// Central differences of the refitted parameters in quote i at h_l = 2^-l min(noise, V / 2).
struct Levels {
  std::array<double, kLevels> h{};
  std::array<bool, kLevels> usable{};
  std::array<Vector5, kLevels> difference{};
};

Levels BumpLevels(const FitSlice& slice, std::size_t i, const FitOptions& options,
                  const std::vector<Bound>& active) {
  Levels out;
  const FitQuote& q = slice.quotes[i];
  for (std::size_t l = 0; l < kLevels; ++l) {
    out.h[l] = std::ldexp(std::min(q.noise, q.price / 2.0), -static_cast<int>(l));
    FitSlice up = slice;
    FitSlice down = slice;
    up.quotes[i].price += out.h[l];
    down.quotes[i].price -= out.h[l];
    const auto u = FitRawSvi(up, options);
    const auto d = FitRawSvi(down, options);
    out.usable[l] = u.has_value() && d.has_value() && u->converged && d->converged &&
                    u->ActiveBounds() == active && d->ActiveBounds() == active;
    if (!out.usable[l]) continue;
    const Vector5 pu = AsVector(u->params);
    const Vector5 pd = AsVector(d->params);
    for (std::size_t j = 0; j < 5; ++j) out.difference[l][j] = (pu[j] - pd[j]) / (2.0 * out.h[l]);
  }
  return out;
}

// slope_factor scales the true slope to give the fit's bound: 0.8 makes it bind.
void BumpAndRefit(const Spec& s, std::uint32_t salt, double slope_factor, std::size_t constraints) {
  const std::vector<double> strikes = GridStrikes(s);
  const FitSlice slice = MakeSlice(s, strikes, true, salt);
  FitOptions options;
  options.slope_bound = slope_factor * s.params.b * (1.0 + std::abs(s.params.rho));
  const auto fit = FitRawSvi(slice, options);
  ASSERT_TRUE(fit.has_value() && fit->converged);
  const std::vector<Bound> active = fit->ActiveBounds();
  ASSERT_EQ(active.size(), constraints);
  const auto adjoint = CalibrationAdjointOf(slice, *fit, kSigmaLeg);
  ASSERT_TRUE(adjoint.has_value()) << AdjointErrorName(adjoint.error().code);

  // The refit error: the largest move of theta-hat under the two alternative grids.
  Vector5 refit{};
  for (const auto& [gm, gs] : {std::pair<std::size_t, std::size_t>{8, 9}, {9, 8}}) {
    FitOptions alt = options;
    alt.grid_m = gm;
    alt.grid_sigma = gs;
    const auto f = FitRawSvi(slice, alt);
    ASSERT_TRUE(f.has_value() && f->converged);
    const Vector5 got = AsVector(f->params);
    const Vector5 want = AsVector(fit->params);
    for (std::size_t j = 0; j < 5; ++j) refit[j] = std::max(refit[j], std::abs(got[j] - want[j]));
  }
  const double kappa = adjoint->exact.condition;
  std::size_t exact_failures = 0;
  std::size_t gauss_newton_failures = 0;
  std::size_t comparisons = 0;
  for (std::size_t i = 0; i < slice.quotes.size(); i += 7) {
    const Levels levels = BumpLevels(slice, i, options, active);
    const auto& [h, usable, difference] = levels;
    double largest = 0.0;
    for (const double v : adjoint->exact.quote[i]) largest = std::max(largest, std::abs(v));
    for (std::size_t j = 0; j < 5; ++j) {
      std::array<double, kLevels> column{};
      for (std::size_t l = 0; l < kLevels; ++l) column[l] = difference[l][j];
      const double floor = kappa * 0x1p-52 * largest;
      const HalvingJudgement e =
          JudgeByHalving(h, column, usable, refit[j], adjoint->exact.quote[i][j], floor);
      // A Gauss-Newton refusal counts as a failure of every comparison.
      const bool g =
          adjoint->gauss_newton &&
          JudgeByHalving(h, column, usable, refit[j], adjoint->gauss_newton->quote[i][j], floor)
              .pass;
      ++comparisons;
      if (!e.pass) ++exact_failures;
      if (!g) ++gauss_newton_failures;
      EXPECT_TRUE(e.pass) << "quote " << i << " parameter " << j << ": " << e.error << " > "
                          << e.tolerance << " at level " << e.level;
    }
  }
  std::printf(
      "bump and refit, %zu active: %zu comparisons, exact %zu failures, Gauss-Newton %s, %zu "
      "failures\n",
      constraints, comparisons, exact_failures,
      adjoint->gauss_newton ? "solved" : AdjointErrorName(adjoint->gauss_newton.error().code),
      gauss_newton_failures);
  EXPECT_GT(gauss_newton_failures, 0U);
}

// Unconstrained, where Gauss-Newton solves and must still disagree somewhere, and on the slope.
TEST(ChainAdjoint, BumpAndRefitAgreesWithTheExactHessianOnly) {
  BumpAndRefit(kA, 1, 4.0e3, 0);
  BumpAndRefit(kC, 2, 0.8, 1);
}

}  // namespace
}  // namespace aadp::svi
