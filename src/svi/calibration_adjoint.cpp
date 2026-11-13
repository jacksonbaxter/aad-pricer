#include "svi/calibration_adjoint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ad/dual.hpp"
#include "ad/second_order.hpp"
#include "svi/arbitrage.hpp"
#include "svi/fit.hpp"
#include "svi/implied_vol.hpp"

namespace aadp::svi {

namespace {

// Lanes: the five parameters, then F, D and sigma_leg.
constexpr std::size_t kLanes = kSviParameters + kMarketInputs;
using D8 = ad::HessianDual<kLanes>;
using R8 = ad::SecondReal<D8>;
using Lane8 = std::array<double, kLanes>;

constexpr std::size_t kForwardLane = kSviParameters;
constexpr std::size_t kDiscountLane = kSviParameters + 1;
constexpr std::size_t kSigmaLegLane = kSviParameters + 2;

// At most three constraints can be active at once (the slope and one rho bound, in practice).
constexpr std::size_t kMaxConstraints = kMaxActiveConstraints;
constexpr std::size_t kMaxKkt = kSviParameters + kMaxConstraints;
using KktMatrix = std::array<std::array<double, kMaxKkt>, kMaxKkt>;
using KktVector = std::array<double, kMaxKkt>;

bool Positive(double x) { return std::isfinite(x) && x > 0.0; }

std::unexpected<AdjointError> Fail(AdjointErrorCode code, std::size_t quote = 0,
                                   Bound bound = Bound::kSlope, double value = 0.0) {
  return std::unexpected(
      AdjointError{.code = code, .quote = quote, .bound = bound, .value = value});
}

D8 Seeded(double value, std::size_t lane) {
  D8 d(value);
  d.tangent[lane] = 1.0;
  return d;
}

bool AllFinite(std::span<const double> v) {
  return std::ranges::all_of(v, [](double x) { return std::isfinite(x); });
}

// One quote's residual r_i on the second-order tape.
R8 Residual(const std::array<R8, kLanes>& z, const R8& price, const FitQuote& q, double tau2,
            double& w_value) {
  const R8& a = z[0];
  const R8& b = z[1];
  const R8& rho = z[2];
  const R8& m = z[3];
  const R8& sigma = z[4];
  const R8& forward = z[kForwardLane];
  const R8& discount = z[kDiscountLane];
  const R8& leg = z[kSigmaLegLane];
  const R8 k = Log(q.strike / forward);
  const R8 x = k - m;
  // Operands that both record are named, in clang's order, so the tape's statement order does not
  // depend on the compiler (src/pricer/kernels.hpp, Finish).
  const R8 rho_x = rho * x;
  const R8 x2 = x * x;
  const R8 sigma2 = sigma * sigma;
  const R8 w = a + (b * (rho_x + Sqrt(x2 + sigma2)));
  w_value = w.value();
  if (!(w_value > 0.0)) return {0.0};
  const R8 root = Sqrt(w);
  const R8 d1 = ((0.5 * w) - k) / root;
  const R8 d2 = d1 - root;
  R8 legs(0.0);
  if (q.type == OptionType::kCall) {
    const R8 share = forward * NormalCdf(d1);
    const R8 cash = q.strike * NormalCdf(d2);
    legs = share - cash;
  } else {
    const R8 cash = q.strike * NormalCdf(-d2);
    const R8 share = forward * NormalCdf(-d1);
    legs = cash - share;
  }
  const R8 model = discount * legs;
  const R8 noise = Sqrt((leg * leg) + tau2);
  return (model - price) / noise;
}

std::expected<void, AdjointError> Validate(const FitSlice& slice, const SviParams& p,
                                           double sigma_leg) {
  if (!Positive(slice.forward) || !Positive(slice.discount) || !Positive(slice.expiry)) {
    return Fail(AdjointErrorCode::kBadSlice);
  }
  if (!std::isfinite(sigma_leg) || sigma_leg < 0.0) return Fail(AdjointErrorCode::kBadSigmaLeg);
  for (std::size_t i = 0; i < slice.quotes.size(); ++i) {
    const FitQuote& q = slice.quotes[i];
    if (!Positive(q.strike) || !Positive(q.price) || !Positive(q.noise) || q.noise < sigma_leg ||
        (q.type != OptionType::kCall && q.type != OptionType::kPut)) {
      return Fail(AdjointErrorCode::kBadQuote, i);
    }
  }
  if (slice.quotes.empty()) return Fail(AdjointErrorCode::kBadQuote);
  const std::array<double, 5> theta = {p.a, p.b, p.rho, p.m, p.sigma};
  if (!AllFinite(theta) || !(p.b > 0.0) || !(std::abs(p.rho) <= 1.0) || !(p.sigma > 0.0)) {
    return Fail(AdjointErrorCode::kBadParameters);
  }
  return {};
}

// A constraint c(theta) <= 0, its gradient and its Hessian in raw parameters.
struct ConstraintModel {
  Bound bound = Bound::kSlope;
  Vector5 gradient{};
  Matrix5 hessian{};
};

std::expected<std::vector<ConstraintModel>, AdjointError> Constraints(std::span<const Bound> active,
                                                                      const SviParams& p) {
  std::vector<ConstraintModel> out;
  for (const Bound bound : active) {
    ConstraintModel c{.bound = bound, .gradient = {}, .hessian = {}};
    switch (bound) {
      case Bound::kSlope: {
        // |rho| has no derivative at 0.
        if (p.rho == 0.0) return Fail(AdjointErrorCode::kUnsupportedBound, 0, bound);
        const double sign = p.rho > 0.0 ? 1.0 : -1.0;
        c.gradient = {0.0, 1.0 + std::abs(p.rho), p.b * sign, 0.0, 0.0};
        c.hessian[1][2] = sign;
        c.hessian[2][1] = sign;
        break;
      }
      case Bound::kRhoUpper:
        c.gradient[2] = 1.0;
        break;
      case Bound::kRhoLower:
        c.gradient[2] = -1.0;
        break;
      default:
        return Fail(AdjointErrorCode::kUnsupportedBound, 0, bound);
    }
    out.push_back(c);
  }
  if (out.size() > kMaxConstraints) return Fail(AdjointErrorCode::kUnsupportedBound);
  return out;
}

// Solves the m x m system in place by Gaussian elimination with partial pivoting.
bool SolveSmall(std::array<std::array<double, kMaxConstraints>, kMaxConstraints>& a,
                std::array<double, kMaxConstraints>& b, std::size_t m) {
  for (std::size_t col = 0; col < m; ++col) {
    std::size_t pivot = col;
    for (std::size_t row = col + 1; row < m; ++row) {
      if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
    }
    if (!(std::abs(a[pivot][col]) > 0.0)) return false;
    std::swap(a[col], a[pivot]);
    std::swap(b[col], b[pivot]);
    for (std::size_t row = col + 1; row < m; ++row) {
      const double f = a[row][col] / a[col][col];
      for (std::size_t j = col; j < m; ++j) a[row][j] -= f * a[col][j];
      b[row] -= f * b[col];
    }
  }
  for (std::size_t col = m; col-- > 0;) {
    double v = b[col];
    for (std::size_t j = col + 1; j < m; ++j) v -= a[col][j] * b[j];
    b[col] = v / a[col][col];
  }
  return std::ranges::all_of(std::span(b).first(m), [](double v) { return std::isfinite(v); });
}

double Norm(std::span<const double> v) {
  double sum = 0.0;
  for (const double x : v) sum += x * x;
  return std::sqrt(sum);
}

// LU factors of a KKT matrix with partial pivoting, P K = L U, stored in place.
struct Lu {
  KktMatrix lu{};
  std::array<std::size_t, kMaxKkt> perm{};
  std::size_t size = 0;

  void Solve(KktVector& x) const {
    KktVector y{};
    for (std::size_t i = 0; i < size; ++i) {
      double v = x[perm[i]];
      for (std::size_t j = 0; j < i; ++j) v -= lu[i][j] * y[j];
      y[i] = v;
    }
    for (std::size_t i = size; i-- > 0;) {
      double v = y[i];
      for (std::size_t j = i + 1; j < size; ++j) v -= lu[i][j] * x[j];
      x[i] = v / lu[i][i];
    }
  }
};

std::expected<Lu, AdjointError> Factor(const KktMatrix& k, std::size_t n) {
  Lu f;
  f.lu = k;
  f.size = n;
  for (std::size_t i = 0; i < n; ++i) f.perm[i] = i;
  for (std::size_t col = 0; col < n; ++col) {
    std::size_t pivot = col;
    for (std::size_t row = col + 1; row < n; ++row) {
      if (std::abs(f.lu[row][col]) > std::abs(f.lu[pivot][col])) pivot = row;
    }
    if (!(std::abs(f.lu[pivot][col]) > 0.0) || !std::isfinite(f.lu[pivot][col])) {
      return Fail(AdjointErrorCode::kSingular);
    }
    std::swap(f.lu[col], f.lu[pivot]);
    std::swap(f.perm[col], f.perm[pivot]);
    for (std::size_t row = col + 1; row < n; ++row) {
      f.lu[row][col] /= f.lu[col][col];
      for (std::size_t j = col + 1; j < n; ++j) f.lu[row][j] -= f.lu[row][col] * f.lu[col][j];
    }
  }
  return f;
}

double OneNorm(const KktMatrix& k, std::size_t n) {
  double best = 0.0;
  for (std::size_t j = 0; j < n; ++j) {
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) sum += std::abs(k[i][j]);
    best = std::max(best, sum);
  }
  return best;
}

// The 1-norm condition number, from the explicit inverse: the matrix is at most 8 x 8.
double Condition(const KktMatrix& k, const Lu& f, std::size_t n) {
  KktMatrix inverse{};
  for (std::size_t j = 0; j < n; ++j) {
    KktVector e{};
    e[j] = 1.0;
    f.Solve(e);
    for (std::size_t i = 0; i < n; ++i) inverse[i][j] = e[i];
  }
  return OneNorm(k, n) * OneNorm(inverse, n);
}

Vector5 Orthogonalised(std::span<const Vector5> q, Vector5 v) {
  for (int pass = 0; pass < 2; ++pass) {
    for (const Vector5& u : q) {
      double dot = 0.0;
      for (std::size_t j = 0; j < 5; ++j) dot += u[j] * v[j];
      for (std::size_t j = 0; j < 5; ++j) v[j] -= dot * u[j];
    }
  }
  return v;
}

// The unused unit vector with the largest part orthogonal to q, normalised; 5 if none is left.
std::size_t BestUnitVector(std::span<const Vector5> q, const std::array<bool, 5>& used,
                           Vector5& best_v) {
  std::size_t best = 5;
  double best_norm = 0.0;
  for (std::size_t i = 0; i < 5; ++i) {
    if (used[i]) continue;
    Vector5 e{};
    e[i] = 1.0;
    const Vector5 v = Orthogonalised(q, e);
    const double n = Norm(v);
    if (n > best_norm) {
      best = i;
      best_norm = n;
      best_v = v;
    }
  }
  if (best < 5) {
    for (double& x : best_v) x /= best_norm;
  }
  return best;
}

// An orthonormal basis of the directions orthogonal to every row of `rows`, by Gram-Schmidt with
// the largest remaining unit vector first.
std::vector<Vector5> NullSpace(std::span<const Vector5> rows) {
  std::vector<Vector5> q;
  for (const Vector5& row : rows) {
    Vector5 v = Orthogonalised(q, row);
    const double n = Norm(v);
    if (!(n > 0.0)) continue;
    for (double& x : v) x /= n;
    q.push_back(v);
  }
  const std::size_t constraints = q.size();
  std::array<bool, 5> used{};
  while (q.size() < 5) {
    Vector5 v{};
    const std::size_t best = BestUnitVector(q, used, v);
    if (best == 5) break;
    used[best] = true;
    q.push_back(v);
  }
  return {q.begin() + static_cast<std::ptrdiff_t>(constraints), q.end()};
}

// Z^T H Z positive definite, by Cholesky.
bool ReducedPositiveDefinite(const Matrix5& h, std::span<const Vector5> z) {
  const std::size_t n = z.size();
  Matrix5 r{};
  for (std::size_t p = 0; p < n; ++p) {
    for (std::size_t q = 0; q < n; ++q) {
      double sum = 0.0;
      for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t j = 0; j < 5; ++j) sum += z[p][i] * h[i][j] * z[q][j];
      }
      r[p][q] = sum;
    }
  }
  for (std::size_t j = 0; j < n; ++j) {
    double d = r[j][j];
    for (std::size_t k = 0; k < j; ++k) d -= r[j][k] * r[j][k];
    if (!(d > 0.0) || !std::isfinite(d)) return false;
    const double l = std::sqrt(d);
    r[j][j] = l;
    for (std::size_t i = j + 1; i < n; ++i) {
      double v = r[i][j];
      for (std::size_t k = 0; k < j; ++k) v -= r[i][k] * r[j][k];
      r[i][j] = v / l;
    }
  }
  return true;
}

// The scaled KKT system of one Hessian, factored once: in u = theta / scale,
//   [S H_L S  S A^T] [u ]   [S top   ]
//   [A S        0  ] [nu] = [  0     ]
// so that d theta = S u and nu is the multiplier's change itself.
class KktSystem {
 public:
  static std::expected<KktSystem, AdjointError> Build(const Matrix5& hessian,
                                                      std::span<const ConstraintModel> constraints,
                                                      const Multipliers& mu, const Vector5& scale,
                                                      const AdjointOptions& options) {
    KktSystem s;
    s.m_ = constraints.size();
    s.n_ = kSviParameters + s.m_;
    s.scale_ = scale;
    Matrix5 lagrangian = hessian;
    for (std::size_t k = 0; k < s.m_; ++k) {
      for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t j = 0; j < 5; ++j) {
          lagrangian[i][j] += mu[k] * constraints[k].hessian[i][j];
        }
      }
    }
    Matrix5 scaled{};
    for (std::size_t i = 0; i < 5; ++i) {
      for (std::size_t j = 0; j < 5; ++j) scaled[i][j] = lagrangian[i][j] * scale[i] * scale[j];
    }
    s.rows_.resize(s.m_);
    for (std::size_t k = 0; k < s.m_; ++k) {
      for (std::size_t j = 0; j < 5; ++j) s.rows_[k][j] = constraints[k].gradient[j] * scale[j];
    }
    if (!ReducedPositiveDefinite(scaled, NullSpace(s.rows_))) {
      return Fail(AdjointErrorCode::kNotMinimum);
    }
    KktMatrix kkt{};
    for (std::size_t i = 0; i < 5; ++i) {
      for (std::size_t j = 0; j < 5; ++j) kkt[i][j] = scaled[i][j];
    }
    for (std::size_t k = 0; k < s.m_; ++k) {
      for (std::size_t j = 0; j < 5; ++j) {
        kkt[5 + k][j] = s.rows_[k][j];
        kkt[j][5 + k] = s.rows_[k][j];
      }
    }
    auto lu = Factor(kkt, s.n_);
    if (!lu) return std::unexpected(lu.error());
    s.lu_ = *lu;
    s.condition_ = Condition(kkt, s.lu_, s.n_);
    if (!std::isfinite(s.condition_) || s.condition_ > options.max_condition) {
      return Fail(AdjointErrorCode::kIllConditioned, 0, Bound::kSlope, s.condition_);
    }
    return s;
  }

  [[nodiscard]] double condition() const { return condition_; }

  // H_L d_theta + A^T d_mu = top, A d_theta = 0. False if anything is not finite.
  bool Solve(const Vector5& top, Vector5& d_theta, Multipliers& d_mu, double& manifold) const {
    KktVector x{};
    for (std::size_t j = 0; j < 5; ++j) x[j] = top[j] * scale_[j];
    lu_.Solve(x);
    for (std::size_t j = 0; j < 5; ++j) d_theta[j] = x[j] * scale_[j];
    d_mu = {};
    for (std::size_t k = 0; k < m_; ++k) d_mu[k] = x[5 + k];
    const double size = Norm(std::span(x).first(5));
    for (std::size_t k = 0; k < m_ && size > 0.0; ++k) {
      double dot = 0.0;
      for (std::size_t j = 0; j < 5; ++j) dot += rows_[k][j] * x[j];
      manifold = std::max(manifold, std::abs(dot) / size);
    }
    return AllFinite(d_theta) && AllFinite(d_mu);
  }

 private:
  Lu lu_;
  std::vector<Vector5> rows_;
  Vector5 scale_{};
  std::size_t m_ = 0;
  std::size_t n_ = 0;
  double condition_ = 0.0;
};

Vector5 Negated(const Vector5& v) {
  Vector5 out{};
  for (std::size_t j = 0; j < 5; ++j) out[j] = -v[j];
  return out;
}

// Every quote's and market input's derivative, for one Hessian.
std::expected<ParameterSensitivity, AdjointError> Sensitivities(
    const KktSystem& kkt, std::span<const Vector5> quote_cross,
    std::span<const Vector5, kMarketInputs> market_cross) {
  ParameterSensitivity out;
  out.condition = kkt.condition();
  out.quote.resize(quote_cross.size());
  out.quote_multiplier.resize(quote_cross.size());
  for (std::size_t i = 0; i < quote_cross.size(); ++i) {
    if (!kkt.Solve(Negated(quote_cross[i]), out.quote[i], out.quote_multiplier[i],
                   out.manifold_residual)) {
      return Fail(AdjointErrorCode::kNonFinite);
    }
  }
  for (std::size_t p = 0; p < kMarketInputs; ++p) {
    if (!kkt.Solve(Negated(market_cross[p]), out.market[p], out.market_multiplier[p],
                   out.manifold_residual)) {
      return Fail(AdjointErrorCode::kNonFinite);
    }
  }
  return out;
}

// The multipliers from a second Newton step, taken at params + step; empty if that point is not
// a valid slice or its system cannot be solved.
std::optional<Multipliers> SecondNewtonStep(const FitSlice& slice, const SviParams& params,
                                            const Vector5& step,
                                            std::span<const ConstraintModel> constraints,
                                            const Multipliers& mu, double sigma_leg,
                                            const AdjointOptions& options) {
  const SviParams moved{.a = params.a + step[0],
                        .b = params.b + step[1],
                        .rho = params.rho + step[2],
                        .m = params.m + step[3],
                        .sigma = params.sigma + step[4]};
  const auto d = DifferentiateObjective(slice, moved, sigma_leg);
  if (!d) return std::nullopt;
  std::vector<Bound> bounds;
  for (const ConstraintModel& c : constraints) bounds.push_back(c.bound);
  const auto moved_constraints = Constraints(bounds, moved);
  if (!moved_constraints) return std::nullopt;
  Vector5 scale{};
  for (std::size_t j = 0; j < 5; ++j) {
    if (!Positive(d->gauss_newton[j][j])) return std::nullopt;
    scale[j] = 1.0 / std::sqrt(d->gauss_newton[j][j]);
  }
  const auto kkt = KktSystem::Build(d->hessian, *moved_constraints, mu, scale, options);
  if (!kkt) return std::nullopt;
  Vector5 unused_step{};
  Multipliers second{};
  double unused = 0.0;
  if (!kkt->Solve(Negated(d->gradient), unused_step, second, unused)) return std::nullopt;
  return second;
}

}  // namespace

const char* AdjointErrorName(AdjointErrorCode code) noexcept {
  switch (code) {
    case AdjointErrorCode::kBadSlice:
      return "bad-slice";
    case AdjointErrorCode::kBadQuote:
      return "bad-quote";
    case AdjointErrorCode::kBadSigmaLeg:
      return "bad-sigma-leg";
    case AdjointErrorCode::kBadParameters:
      return "bad-parameters";
    case AdjointErrorCode::kNonPositiveVariance:
      return "non-positive-variance";
    case AdjointErrorCode::kNonFinite:
      return "non-finite";
    case AdjointErrorCode::kUnsupportedBound:
      return "unsupported-bound";
    case AdjointErrorCode::kDegenerateMultiplier:
      return "degenerate-multiplier";
    case AdjointErrorCode::kNotMinimum:
      return "not-minimum";
    case AdjointErrorCode::kSingular:
      return "singular";
    case AdjointErrorCode::kIllConditioned:
      return "ill-conditioned";
  }
  return "unknown";
}

namespace {

bool Finite(const ObjectiveDerivatives& d) {
  bool finite = std::isfinite(d.objective) && AllFinite(d.gradient);
  for (std::size_t j = 0; j < 5; ++j) {
    finite = finite && AllFinite(d.hessian[j]) && AllFinite(d.gauss_newton[j]);
  }
  for (const Vector5& v : d.quote_cross) finite = finite && AllFinite(v);
  for (std::size_t p = 0; p < kMarketInputs; ++p) {
    finite = finite && AllFinite(d.market_cross[p]) && AllFinite(d.market_cross_gauss_newton[p]);
  }
  return finite;
}

// 2 J^T J over all eight lanes.
std::array<Lane8, kLanes> GaussNewton(std::span<const Lane8> jacobian) {
  std::array<Lane8, kLanes> gn{};
  for (const Lane8& row : jacobian) {
    for (std::size_t j = 0; j < kLanes; ++j) {
      for (std::size_t k = 0; k < kLanes; ++k) gn[j][k] += 2.0 * row[j] * row[k];
    }
  }
  return gn;
}

// The parameter block: the gradient, H symmetrised with its asymmetry, and GN.
void ReadParameterBlock(const ad::SecondTape<D8>& tape, std::span<const R8, kLanes> z,
                        const std::array<Lane8, kLanes>& gn, ObjectiveDerivatives& out) {
  Matrix5 raw{};
  for (std::size_t j = 0; j < 5; ++j) {
    const D8& adjoint = tape.Adjoint(z[j]);
    out.gradient[j] = adjoint.value;
    for (std::size_t k = 0; k < 5; ++k) {
      raw[j][k] = adjoint.tangent[k];
      out.gauss_newton[j][k] = gn[j][k];
    }
  }
  for (std::size_t j = 0; j < 5; ++j) {
    for (std::size_t k = 0; k < 5; ++k) {
      out.hessian[j][k] = 0.5 * (raw[j][k] + raw[k][j]);
      const double scale = std::sqrt(gn[j][j] * gn[k][k]);
      if (scale > 0.0) {
        out.hessian_asymmetry =
            std::max(out.hessian_asymmetry, std::abs(raw[j][k] - raw[k][j]) / scale);
      }
    }
  }
}

}  // namespace

std::expected<ObjectiveDerivatives, AdjointError> DifferentiateObjective(const FitSlice& slice,
                                                                         const SviParams& params,
                                                                         double sigma_leg) {
  if (auto valid = Validate(slice, params, sigma_leg); !valid) {
    return std::unexpected(valid.error());
  }
  const std::size_t n = slice.quotes.size();
  ad::SecondTape<D8> tape;
  const ad::SecondTapeScope<D8> scope(tape);
  const Lane8 values = {params.a,     params.b,      params.rho,     params.m,
                        params.sigma, slice.forward, slice.discount, sigma_leg};
  std::array<R8, kLanes> z{};
  for (std::size_t j = 0; j < kLanes; ++j) z[j] = tape.Register(Seeded(values[j], j));
  std::vector<R8> price(n);
  for (std::size_t i = 0; i < n; ++i) price[i] = tape.Register(D8(slice.quotes[i].price));

  std::vector<Lane8> jacobian(n);
  R8 phi(0.0);
  for (std::size_t i = 0; i < n; ++i) {
    const FitQuote& q = slice.quotes[i];
    const double tau2 = std::max((q.noise * q.noise) - (sigma_leg * sigma_leg), 0.0);
    double w = 0.0;
    const R8 r = Residual(z, price[i], q, tau2, w);
    if (!(w > 0.0)) return Fail(AdjointErrorCode::kNonPositiveVariance, i);
    jacobian[i] = r.dual().tangent;
    phi = phi + (r * r);
  }
  tape.AddAdjoint(phi, 1.0);
  tape.SweepBack(tape.GetPosition(), ad::SecondPosition{});

  ObjectiveDerivatives out;
  out.objective = phi.value();
  const std::array<Lane8, kLanes> gn = GaussNewton(jacobian);
  ReadParameterBlock(tape, z, gn, out);
  out.quote_cross.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    const D8& adjoint = tape.Adjoint(price[i]);
    for (std::size_t k = 0; k < 5; ++k) out.quote_cross[i][k] = adjoint.tangent[k];
  }
  for (std::size_t p = 0; p < kMarketInputs; ++p) {
    const D8& adjoint = tape.Adjoint(z[kSviParameters + p]);
    for (std::size_t k = 0; k < 5; ++k) {
      out.market_cross[p][k] = adjoint.tangent[k];
      out.market_cross_gauss_newton[p][k] = gn[kSviParameters + p][k];
    }
  }
  if (!Finite(out)) return Fail(AdjointErrorCode::kNonFinite);
  return out;
}

namespace {

// 1 / sqrt(GN_jj); empty if a diagonal entry is not positive.
std::optional<Vector5> Scale(const Matrix5& gauss_newton) {
  Vector5 scale{};
  for (std::size_t j = 0; j < 5; ++j) {
    if (!Positive(gauss_newton[j][j])) return std::nullopt;
    scale[j] = 1.0 / std::sqrt(gauss_newton[j][j]);
  }
  return scale;
}

// First-order multipliers: least squares on the scaled stationarity condition. Also sets the
// stationarity and gradient norms.
std::expected<Multipliers, AdjointError> LeastSquaresMultipliers(
    const ObjectiveDerivatives& d, std::span<const ConstraintModel> constraints,
    const Vector5& scale, CalibrationAdjoint& out) {
  const std::size_t m = constraints.size();
  Vector5 g{};
  for (std::size_t j = 0; j < 5; ++j) g[j] = d.gradient[j] * scale[j];
  std::array<Vector5, kMaxConstraints> a{};
  for (std::size_t k = 0; k < m; ++k) {
    for (std::size_t j = 0; j < 5; ++j) a[k][j] = constraints[k].gradient[j] * scale[j];
  }
  std::array<std::array<double, kMaxConstraints>, kMaxConstraints> normal{};
  Multipliers least_squares{};
  for (std::size_t k = 0; k < m; ++k) {
    for (std::size_t l = 0; l < m; ++l) {
      for (std::size_t j = 0; j < 5; ++j) normal[k][l] += a[k][j] * a[l][j];
    }
    for (std::size_t j = 0; j < 5; ++j) least_squares[k] -= a[k][j] * g[j];
  }
  if (m > 0 && !SolveSmall(normal, least_squares, m)) return Fail(AdjointErrorCode::kSingular);
  Vector5 residual = g;
  for (std::size_t k = 0; k < m; ++k) {
    for (std::size_t j = 0; j < 5; ++j) residual[j] += least_squares[k] * a[k][j];
  }
  out.stationarity = Norm(residual);
  out.gradient_norm = Norm(g);
  return least_squares;
}

// Constraint k's significance and quote distance, from the exact d mu / d V.
void Significance(const FitSlice& slice, double mu, std::size_t k, CalibrationAdjoint& out) {
  double variance = 0.0;
  double distance = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < slice.quotes.size(); ++i) {
    const double move = std::abs(out.exact.quote_multiplier[i][k]) * slice.quotes[i].noise;
    variance += move * move;
    if (move > 0.0) distance = std::min(distance, mu / move);
  }
  out.constraints[k].significance = mu / std::sqrt(variance);
  out.constraints[k].quote_distance = distance;
}

}  // namespace

std::expected<CalibrationAdjoint, AdjointError> CalibrationAdjointAt(
    const FitSlice& slice, const SviParams& params, std::span<const Bound> active, double sigma_leg,
    const AdjointOptions& options) {
  auto derivatives = DifferentiateObjective(slice, params, sigma_leg);
  if (!derivatives) return std::unexpected(derivatives.error());
  const auto constraints = Constraints(active, params);
  if (!constraints) return std::unexpected(constraints.error());

  CalibrationAdjoint out;
  out.derivatives = std::move(*derivatives);
  const ObjectiveDerivatives& d = out.derivatives;
  const std::optional<Vector5> scaling = Scale(d.gauss_newton);
  if (!scaling) return Fail(AdjointErrorCode::kSingular);
  const Vector5& scale = *scaling;

  const std::size_t m = constraints->size();
  const auto first_order = LeastSquaresMultipliers(d, *constraints, scale, out);
  if (!first_order) return std::unexpected(first_order.error());
  const Multipliers& least_squares = *first_order;

  // One Newton step on the KKT system gives the multiplier at the exact KKT point; then the
  // system is rebuilt with it, since H_L carries mu.
  auto first = KktSystem::Build(d.hessian, *constraints, least_squares, scale, options);
  if (!first) return std::unexpected(first.error());
  Multipliers mu{};
  double unused = 0.0;
  if (!first->Solve(Negated(d.gradient), out.newton_step, mu, unused)) {
    return Fail(AdjointErrorCode::kNonFinite);
  }
  auto kkt = KktSystem::Build(d.hessian, *constraints, mu, scale, options);
  if (!kkt) return std::unexpected(kkt.error());
  const std::optional<Multipliers> second =
      m > 0 ? SecondNewtonStep(slice, params, out.newton_step, *constraints, mu, sigma_leg, options)
            : std::nullopt;
  for (std::size_t k = 0; k < m; ++k) {
    const double reference = second ? (*second)[k] : least_squares[k];
    const double margin =
        mu[k] / std::max(std::abs(mu[k] - reference), std::numeric_limits<double>::min());
    out.constraints.push_back({.bound = (*constraints)[k].bound,
                               .multiplier = mu[k],
                               .multiplier_least_squares = least_squares[k],
                               .multiplier_second_step = second ? (*second)[k] : 0.0,
                               .margin = margin,
                               .significance = 0.0,
                               .quote_distance = 0.0});
    if (!(mu[k] > 0.0) || !(margin >= options.min_multiplier_margin)) {
      return Fail(AdjointErrorCode::kDegenerateMultiplier, 0, (*constraints)[k].bound, margin);
    }
  }

  auto exact = Sensitivities(*kkt, d.quote_cross, d.market_cross);
  if (!exact) return std::unexpected(exact.error());
  out.exact = std::move(*exact);
  for (std::size_t k = 0; k < m; ++k) Significance(slice, mu[k], k, out);

  auto gauss_newton = KktSystem::Build(d.gauss_newton, *constraints, mu, scale, options);
  if (gauss_newton) {
    out.gauss_newton = Sensitivities(*gauss_newton, d.quote_cross, d.market_cross_gauss_newton);
  } else {
    out.gauss_newton = std::unexpected(gauss_newton.error());
  }
  return out;
}

std::expected<CalibrationAdjoint, AdjointError> CalibrationAdjointOf(
    const FitSlice& slice, const FitResult& fit, double sigma_leg, const AdjointOptions& options) {
  const std::vector<Bound> active = fit.ActiveBounds();
  return CalibrationAdjointAt(slice, fit.params, active, sigma_leg, options);
}

}  // namespace aadp::svi
