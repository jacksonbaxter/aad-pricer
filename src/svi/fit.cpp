#include "svi/fit.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <expected>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "svi/arbitrage.hpp"
#include "svi/implied_vol.hpp"
#include "svi/raw_svi.hpp"

namespace aadp::svi {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t kMinStrikes = 5;
// A quote whose J_i is below this drops out of one Gauss-Newton step, never out of Phi.
constexpr double kTinyJacobian = 1e-300;
constexpr int kMaxHalvings = 40;
// Pivot floor of the scaled KKT systems, whose entries are O(1).
constexpr double kPivotFloor = 1e-13;
// Relative feasibility slack of a QP candidate's inactive constraints.
constexpr double kFeasibility = 1e-12;
constexpr double kStartsAtBest = 1e-8;

// The slice, checked and inverted once.
struct Prepared {
  double forward = 0.0;
  double discount = 0.0;
  double expiry = 0.0;
  std::vector<double> k;
  std::vector<double> strike;
  std::vector<double> price;
  std::vector<double> inv_noise;
  std::vector<OptionType> type;
  std::vector<double> market_w;
  std::vector<double> market_vol;
  double k_min = 0.0;
  double k_max = 0.0;
  double w_min = 0.0;
  double w_max = 0.0;
};

bool Positive(double x) { return std::isfinite(x) && x > 0.0; }

std::expected<Prepared, FitError> Prepare(const FitSlice& slice) {
  if (!Positive(slice.forward) || !Positive(slice.discount) || !Positive(slice.expiry)) {
    return std::unexpected(FitError{.code = FitErrorCode::kBadSlice});
  }
  Prepared p;
  p.forward = slice.forward;
  p.discount = slice.discount;
  p.expiry = slice.expiry;
  const std::size_t n = slice.quotes.size();
  for (std::size_t i = 0; i < n; ++i) {
    const FitQuote& q = slice.quotes[i];
    if (!Positive(q.strike) || !Positive(q.price) || !Positive(q.noise) ||
        (q.type != OptionType::kCall && q.type != OptionType::kPut)) {
      return std::unexpected(FitError{.code = FitErrorCode::kBadQuote, .quote = i});
    }
  }
  std::vector<double> strikes;
  strikes.reserve(n);
  for (const FitQuote& q : slice.quotes) strikes.push_back(q.strike);
  std::ranges::sort(strikes);
  const auto distinct = static_cast<std::size_t>(
      std::distance(strikes.begin(), std::ranges::unique(strikes).begin()));
  if (distinct < kMinStrikes) {
    return std::unexpected(FitError{.code = FitErrorCode::kTooFewStrikes});
  }
  p.k.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const FitQuote& q = slice.quotes[i];
    const ImpliedVol iv = ImpliedVolatility({.forward = slice.forward,
                                             .discount = slice.discount,
                                             .strike = q.strike,
                                             .expiry = slice.expiry,
                                             .price = q.price,
                                             .type = q.type});
    if (iv.status != InversionStatus::kOk) {
      return std::unexpected(
          FitError{.code = FitErrorCode::kNoImpliedVol, .quote = i, .status = iv.status});
    }
    const double kk = std::log(q.strike / slice.forward);
    const double w = iv.vol * iv.vol * slice.expiry;
    if (!std::isfinite(kk) || !Positive(w)) {
      return std::unexpected(FitError{.code = FitErrorCode::kNoImpliedVol,
                                      .quote = i,
                                      .status = InversionStatus::kNotAttained});
    }
    p.k.push_back(kk);
    p.strike.push_back(q.strike);
    p.price.push_back(q.price);
    p.inv_noise.push_back(1.0 / q.noise);
    p.type.push_back(q.type);
    p.market_w.push_back(w);
    p.market_vol.push_back(iv.vol);
  }
  p.k_min = *std::ranges::min_element(p.k);
  p.k_max = *std::ranges::max_element(p.k);
  p.w_min = *std::ranges::min_element(p.market_w);
  p.w_max = *std::ranges::max_element(p.market_w);
  return p;
}

double ModelPrice(const Prepared& p, std::size_t i, double w) {
  return Black76Price(p.forward, p.discount, p.strike[i], p.expiry, std::sqrt(w / p.expiry),
                      p.type[i]);
}

// dV/dw = D F phi(d1) / (2 sqrt(w)) for calls and puts alike: C - P does not depend on w.
double PriceSlope(const Prepared& p, std::size_t i, double w) {
  const double s = std::sqrt(w);
  const double d1 = (-p.k[i] + (0.5 * w)) / s;
  const double phi =
      std::exp(-0.5 * d1 * d1) * 0.5 * std::numbers::sqrt2 * std::numbers::inv_sqrtpi;
  return p.discount * p.forward * phi / (2.0 * s);
}

// The quasi-explicit coordinates at one (m, sigma).
struct Coordinates {
  double m = 0.0;
  double sigma = 0.0;
  std::vector<double> y;
  std::vector<double> z;
};

void SetCoordinates(const Prepared& p, double m, double sigma, Coordinates& c) {
  c.m = m;
  c.sigma = sigma;
  const std::size_t n = p.k.size();
  c.y.resize(n);
  c.z.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    c.y[i] = (p.k[i] - m) / sigma;
    c.z[i] = std::sqrt((c.y[i] * c.y[i]) + 1.0);
  }
}

// x = (a, d, c).
using Vec3 = std::array<double, 3>;

double MinVariance(const Vec3& x) {
  return x[0] + std::sqrt(std::max((x[2] * x[2]) - (x[1] * x[1]), 0.0));
}

bool Admissible(const Vec3& x) {
  return std::isfinite(x[0]) && std::isfinite(x[1]) && std::isfinite(x[2]) && MinVariance(x) > 0.0;
}

// Phi at x for the current coordinates, or +inf where it is not defined.
double Phi(const Prepared& p, const Coordinates& c, const Vec3& x) {
  if (!Admissible(x)) return kInf;
  double sum = 0.0;
  for (std::size_t i = 0; i < p.k.size(); ++i) {
    const double w = x[0] + (x[1] * c.y[i]) + (x[2] * c.z[i]);
    if (!(w > 0.0)) return kInf;
    const double r = (ModelPrice(p, i, w) - p.price[i]) * p.inv_noise[i];
    sum += r * r;
  }
  return std::isfinite(sum) ? sum : kInf;
}

// The five linear constraints, A x <= b, in the order of the Bound enum's first entries.
constexpr std::size_t kConstraints = 5;
struct Constraints {
  std::array<Vec3, kConstraints> row{};
  std::array<double, kConstraints> rhs{};
};

Constraints MakeConstraints(double sigma, double slope_bound, double a_max) {
  const double s = slope_bound * sigma;
  return {.row = {{{0.0, -1.0, -1.0},  // rho >= -1: c + d >= 0
                   {0.0, 1.0, -1.0},   // rho <= +1: c - d >= 0
                   {0.0, 1.0, 1.0},    // c + d <= s
                   {0.0, -1.0, 1.0},   // c - d <= s
                   {1.0, 0.0, 0.0}}},  // a <= a_max
          .rhs = {0.0, 0.0, s, s, a_max}};
}

// Solves M v = r in place by Gaussian elimination with partial pivoting. False if singular.
template <std::size_t N>
bool Solve(std::array<std::array<double, N>, N>& m, std::array<double, N>& r, std::size_t size) {
  for (std::size_t col = 0; col < size; ++col) {
    std::size_t pivot = col;
    for (std::size_t row = col + 1; row < size; ++row) {
      if (std::abs(m[row][col]) > std::abs(m[pivot][col])) pivot = row;
    }
    if (!(std::abs(m[pivot][col]) > kPivotFloor)) return false;
    std::swap(m[col], m[pivot]);
    std::swap(r[col], r[pivot]);
    for (std::size_t row = col + 1; row < size; ++row) {
      const double f = m[row][col] / m[col][col];
      for (std::size_t j = col; j < size; ++j) m[row][j] -= f * m[col][j];
      r[row] -= f * r[col];
    }
  }
  for (std::size_t col = size; col-- > 0;) {
    double v = r[col];
    for (std::size_t j = col + 1; j < size; ++j) v -= m[col][j] * r[j];
    r[col] = v / m[col][col];
  }
  return std::ranges::all_of(std::span(r).first(size), [](double v) { return std::isfinite(v); });
}

// Sets the active constraints of a candidate exactly, so that rho = -1 is -1 as a double.
// Returns false for a set that cannot hold at once (parallel rows, or three of the first four).
bool Snap(unsigned mask, const Constraints& cons, Vec3& x) {
  const double s = cons.rhs[2];
  const unsigned rho_mask = mask & 0xFU;
  switch (rho_mask) {
    case 0x0U:
      break;
    case 0x1U:
      x[1] = -x[2];
      break;
    case 0x2U:
      x[1] = x[2];
      break;
    case 0x4U:
      x[1] = s - x[2];
      break;
    case 0x8U:
      x[1] = x[2] - s;
      break;
    case 0x3U:
      x[1] = 0.0;
      x[2] = 0.0;
      break;
    case 0x9U:
      x[2] = 0.5 * s;
      x[1] = -0.5 * s;
      break;
    case 0x6U:
      x[2] = 0.5 * s;
      x[1] = 0.5 * s;
      break;
    case 0xCU:
      x[2] = s;
      x[1] = 0.0;
      break;
    default:
      return false;
  }
  if ((mask & 0x10U) != 0U) x[0] = cons.rhs[4];
  return true;
}

// The normal equations of sum lambda_i (t_i - a - d y_i - c z_i)^2, with x Jacobi-scaled and
// each constraint row scaled to unit length, so that every KKT entry is O(1).
struct ScaledQp {
  std::array<std::array<double, 3>, 3> h{};
  Vec3 g{};
  Vec3 scale{};
  std::array<Vec3, kConstraints> rows{};
  std::array<double, kConstraints> rhs{};
};

std::optional<ScaledQp> ScaleQp(const Coordinates& co, std::span<const double> lambda,
                                std::span<const double> target, const Constraints& cons) {
  std::array<std::array<double, 3>, 3> h{};
  Vec3 g{};
  for (std::size_t i = 0; i < lambda.size(); ++i) {
    const Vec3 phi = {1.0, co.y[i], co.z[i]};
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) h[r][c] += lambda[i] * phi[r] * phi[c];
      g[r] += lambda[i] * target[i] * phi[r];
    }
  }
  ScaledQp qp;
  for (std::size_t j = 0; j < 3; ++j) {
    if (!(h[j][j] > 0.0) || !std::isfinite(h[j][j])) return std::nullopt;
    qp.scale[j] = 1.0 / std::sqrt(h[j][j]);
  }
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) qp.h[r][c] = h[r][c] * qp.scale[r] * qp.scale[c];
    qp.g[r] = g[r] * qp.scale[r];
  }
  for (std::size_t c = 0; c < kConstraints; ++c) {
    double norm = 0.0;
    for (std::size_t j = 0; j < 3; ++j) {
      qp.rows[c][j] = cons.row[c][j] * qp.scale[j];
      norm += qp.rows[c][j] * qp.rows[c][j];
    }
    norm = std::sqrt(norm);
    for (std::size_t j = 0; j < 3; ++j) qp.rows[c][j] /= norm;
    qp.rhs[c] = cons.rhs[c] / norm;
  }
  return qp;
}

// The minimiser on the face where the constraints in mask hold with equality, or empty if that
// KKT system is singular or the face cannot exist.
std::optional<Vec3> FaceMinimiser(const ScaledQp& qp, unsigned mask, const Constraints& cons) {
  constexpr std::size_t kMax = 3 + kConstraints;
  std::array<std::array<double, kMax>, kMax> kkt{};
  std::array<double, kMax> v{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) kkt[r][c] = qp.h[r][c];
    v[r] = qp.g[r];
  }
  std::size_t row = 3;
  for (std::size_t c = 0; c < kConstraints; ++c) {
    if ((mask & (1U << c)) == 0U) continue;
    for (std::size_t j = 0; j < 3; ++j) {
      kkt[row][j] = qp.rows[c][j];
      kkt[j][row] = qp.rows[c][j];
    }
    v[row] = qp.rhs[c];
    ++row;
  }
  if (!Solve(kkt, v, row)) return std::nullopt;
  Vec3 x = {v[0] * qp.scale[0], v[1] * qp.scale[1], v[2] * qp.scale[2]};
  if (!Snap(mask, cons, x)) return std::nullopt;
  return x;
}

bool Feasible(const Constraints& cons, const Vec3& x) {
  for (std::size_t c = 0; c < kConstraints; ++c) {
    const double lhs = (cons.row[c][0] * x[0]) + (cons.row[c][1] * x[1]) + (cons.row[c][2] * x[2]);
    const double tolerance = kFeasibility * std::max({std::abs(cons.rhs[c]), cons.rhs[2], 1e-300});
    if (!(lhs <= cons.rhs[c] + tolerance)) return false;
  }
  return true;
}

double QpValue(const Coordinates& co, std::span<const double> lambda,
               std::span<const double> target, const Vec3& x) {
  double value = 0.0;
  for (std::size_t i = 0; i < lambda.size(); ++i) {
    const double e = target[i] - (x[0] + (x[1] * co.y[i]) + (x[2] * co.z[i]));
    value += lambda[i] * e * e;
  }
  return value;
}

// min sum lambda_i (t_i - a - d y_i - c z_i)^2 subject to A x <= b, by every set of at most three
// active constraints. The best feasible candidate is the minimiser: each one minimises the
// objective on its face, and the minimiser lies on one face and is feasible there.
std::optional<Vec3> SolveQp(const Coordinates& co, std::span<const double> lambda,
                            std::span<const double> target, const Constraints& cons) {
  const std::optional<ScaledQp> qp = ScaleQp(co, lambda, target, cons);
  if (!qp) return std::nullopt;
  std::optional<Vec3> best;
  double best_value = kInf;
  for (unsigned mask = 0; mask < (1U << kConstraints); ++mask) {
    if (std::popcount(mask) > 3) continue;
    const std::optional<Vec3> x = FaceMinimiser(*qp, mask, cons);
    if (!x || !Feasible(cons, *x)) continue;
    const double value = QpValue(co, lambda, target, *x);
    if (std::isfinite(value) && value < best_value) {
      best_value = value;
      best = x;
    }
  }
  return best;
}

struct InnerResult {
  Vec3 x{};
  double phi = kInf;
  bool converged = false;
};

// One Gauss-Newton linearisation at x: lambda_i = J_i^2 and t_i = w_i - r_i / J_i.
void Linearise(const Prepared& p, const Coordinates& co, const Vec3& x, std::vector<double>& lambda,
               std::vector<double>& target) {
  for (std::size_t i = 0; i < p.k.size(); ++i) {
    const double w = x[0] + (x[1] * co.y[i]) + (x[2] * co.z[i]);
    const double r = (ModelPrice(p, i, w) - p.price[i]) * p.inv_noise[i];
    const double j = PriceSlope(p, i, w) * p.inv_noise[i];
    if (std::isfinite(j) && j > kTinyJacobian && std::isfinite(r)) {
      lambda[i] = j * j;
      target[i] = w - (r / j);
    } else {
      lambda[i] = 0.0;
      target[i] = w;
    }
  }
}

// Backtracking from x towards the QP's point, alpha = 1, 1/2, ... 2^-kMaxHalvings, on Phi and
// positivity. Empty if no alpha decreases Phi.
std::optional<InnerResult> LineSearch(const Prepared& p, const Coordinates& co,
                                      const InnerResult& at, const Vec3& toward) {
  double alpha = 1.0;
  for (int halving = 0; halving <= kMaxHalvings; ++halving) {
    Vec3 next = toward;
    if (halving > 0) {
      for (std::size_t j = 0; j < 3; ++j) next[j] = at.x[j] + (alpha * (toward[j] - at.x[j]));
    }
    const double next_phi = Phi(p, co, next);
    if (next_phi < at.phi) return InnerResult{.x = next, .phi = next_phi, .converged = false};
    alpha *= 0.5;
  }
  return std::nullopt;
}

// The first quasi-explicit solve, at the market variances where r = 0, if it is admissible;
// otherwise the flat slice at the smallest market variance.
InnerResult Start(const Prepared& p, const Coordinates& co, const Constraints& cons,
                  std::vector<double>& lambda, std::vector<double>& target) {
  for (std::size_t i = 0; i < p.k.size(); ++i) {
    const double j = PriceSlope(p, i, p.market_w[i]) * p.inv_noise[i];
    lambda[i] = std::isfinite(j) && j > kTinyJacobian ? j * j : 0.0;
    target[i] = p.market_w[i];
  }
  const std::optional<Vec3> first = SolveQp(co, lambda, target, cons);
  const Vec3 flat = {p.w_min, 0.0, 0.0};
  InnerResult out;
  out.x = first && Admissible(*first) ? *first : flat;
  out.phi = Phi(p, co, out.x);
  if (!std::isfinite(out.phi)) {
    out.x = flat;
    out.phi = Phi(p, co, out.x);
  }
  return out;
}

// Gauss-Newton on Phi in (a, d, c) for fixed (m, sigma), from the quasi-explicit solve at the
// market variances.
InnerResult SolveInner(const Prepared& p, const Coordinates& co, const FitOptions& options) {
  const Constraints cons = MakeConstraints(co.sigma, options.slope_bound, p.w_max);
  std::vector<double> lambda(p.k.size());
  std::vector<double> target(p.k.size());
  InnerResult out = Start(p, co, cons, lambda, target);
  if (!std::isfinite(out.phi)) return out;
  for (std::size_t step = 0; step < options.max_inner_steps; ++step) {
    Linearise(p, co, out.x, lambda, target);
    const std::optional<Vec3> qp = SolveQp(co, lambda, target, cons);
    // No step defined, or no decrease left at this precision: the point stands.
    const std::optional<InnerResult> next = qp ? LineSearch(p, co, out, *qp) : std::nullopt;
    if (!next) {
      out.converged = true;
      return out;
    }
    const double decrease = out.phi - next->phi;
    const double before = out.phi;
    out = *next;
    if (decrease <= options.inner_relative_decrease * before) {
      out.converged = true;
      return out;
    }
  }
  return out;
}

struct Profile {
  const Prepared* p = nullptr;
  const FitOptions* options = nullptr;
  double m_lo = 0.0;
  double m_hi = 0.0;
  double u_lo = 0.0;
  double u_hi = 0.0;
  std::size_t evaluations = 0;
  Coordinates co;

  double operator()(double m, double u) {
    ++evaluations;
    SetCoordinates(*p, m, std::exp(u), co);
    return SolveInner(*p, co, *options).phi;
  }
};

struct Vertex {
  double m = 0.0;
  double u = 0.0;
  double f = kInf;
};

struct StartResult {
  Vertex best;
  bool converged = false;
};

// Nelder-Mead in (m, u = ln sigma) with coefficients 1, 2, 1/2, 1/2, every trial point clamped
// to the box.
class Simplex {
 public:
  explicit Simplex(Profile& profile) : profile_(&profile) {}

  Vertex Make(double m, double u) {
    const double mc = std::clamp(m, profile_->m_lo, profile_->m_hi);
    const double uc = std::clamp(u, profile_->u_lo, profile_->u_hi);
    return Vertex{.m = mc, .u = uc, .f = (*profile_)(mc, uc)};
  }

  // One iteration on v, sorted by f, best first.
  void Step(std::array<Vertex, 3>& v) {
    const double cm = 0.5 * (v[0].m + v[1].m);
    const double cu = 0.5 * (v[0].u + v[1].u);
    const Vertex reflected = Make(cm + (cm - v[2].m), cu + (cu - v[2].u));
    if (reflected.f < v[0].f) {
      const Vertex expanded = Make(cm + (2.0 * (cm - v[2].m)), cu + (2.0 * (cu - v[2].u)));
      v[2] = expanded.f < reflected.f ? expanded : reflected;
      return;
    }
    if (reflected.f < v[1].f) {
      v[2] = reflected;
      return;
    }
    if (const std::optional<Vertex> contracted = Contract(v[2], reflected, cm, cu)) {
      v[2] = *contracted;
      return;
    }
    for (std::size_t i = 1; i < 3; ++i) {
      v[i] = Make(v[0].m + (0.5 * (v[i].m - v[0].m)), v[0].u + (0.5 * (v[i].u - v[0].u)));
    }
  }

 private:
  // Outside contraction when the reflection beat the worst point, inside otherwise. Empty means
  // shrink.
  std::optional<Vertex> Contract(const Vertex& worst, const Vertex& reflected, double cm,
                                 double cu) {
    if (reflected.f < worst.f) {
      const Vertex outside = Make(cm + (0.5 * (reflected.m - cm)), cu + (0.5 * (reflected.u - cu)));
      if (outside.f <= reflected.f) return outside;
      return std::nullopt;
    }
    const Vertex inside = Make(cm + (0.5 * (worst.m - cm)), cu + (0.5 * (worst.u - cu)));
    if (inside.f < worst.f) return inside;
    return std::nullopt;
  }

  Profile* profile_;
};

bool SimplexConverged(const std::array<Vertex, 3>& v, double tolerance) {
  const double spread = std::max({std::abs(v[1].m - v[0].m), std::abs(v[2].m - v[0].m),
                                  std::abs(v[1].u - v[0].u), std::abs(v[2].u - v[0].u)});
  return std::isfinite(v[0].f) && (spread <= tolerance || (v[0].f == v[1].f && v[1].f == v[2].f));
}

StartResult NelderMead(Profile& profile, Vertex start, double hm, double hu) {
  const std::size_t budget = profile.evaluations + profile.options->max_evaluations_per_start;
  Simplex simplex(profile);
  std::array<Vertex, 3> v{};
  v[0] = start;
  const double m1 = start.m + hm <= profile.m_hi ? start.m + hm : start.m - hm;
  const double u2 = start.u + hu <= profile.u_hi ? start.u + hu : start.u - hu;
  v[1] = simplex.Make(m1, start.u);
  v[2] = simplex.Make(start.m, u2);
  StartResult out;
  while (true) {
    // Stable, so tied vertices keep their order with any standard library.
    std::ranges::stable_sort(v, {}, &Vertex::f);
    out.best = v[0];
    if (SimplexConverged(v, profile.options->simplex_tolerance)) {
      out.converged = true;
      return out;
    }
    if (profile.evaluations >= budget) return out;
    simplex.Step(v);
  }
}

SviParams ToRaw(const Vec3& x, double m, double sigma) {
  const double c = x[2];
  const double rho = c > 0.0 ? std::clamp(x[1] / c, -1.0, 1.0) : 0.0;
  return {.a = x[0], .b = c / sigma, .rho = rho, .m = m, .sigma = sigma};
}

void FillBounds(FitResult& r, const FitOptions& o) {
  const SviParams& q = r.params;
  const double slope = q.b * (1.0 + std::abs(q.rho));
  const double min_w = q.a + (q.b * q.sigma * std::sqrt(std::max(1.0 - (q.rho * q.rho), 0.0)));
  const double m_width = r.m_hi - r.m_lo;
  const double u_width = std::log(r.sigma_hi / r.sigma_lo);
  const auto state = [](Bound bound, double slack, double scale) {
    return BoundState{.bound = bound, .slack = slack, .active = slack <= kActiveTolerance * scale};
  };
  r.bounds = {state(Bound::kRhoLower, 1.0 + q.rho, 1.0),
              state(Bound::kRhoUpper, 1.0 - q.rho, 1.0),
              state(Bound::kBZero, q.b, r.a_max / q.sigma),
              state(Bound::kSlope, o.slope_bound - slope, o.slope_bound),
              state(Bound::kAUpper, r.a_max - q.a, r.a_max),
              state(Bound::kMLower, q.m - r.m_lo, m_width),
              state(Bound::kMUpper, r.m_hi - q.m, m_width),
              state(Bound::kSigmaLower, std::log(q.sigma / r.sigma_lo), u_width),
              state(Bound::kSigmaUpper, std::log(r.sigma_hi / q.sigma), u_width),
              state(Bound::kMinVariance, min_w, r.a_max)};
}

FitOptions Sanitised(FitOptions o) {
  o.grid_m = std::max<std::size_t>(o.grid_m, 2);
  o.grid_sigma = std::max<std::size_t>(o.grid_sigma, 2);
  o.starts = std::clamp<std::size_t>(o.starts, 1, o.grid_m * o.grid_sigma);
  o.max_evaluations_per_start = std::max<std::size_t>(o.max_evaluations_per_start, 3);
  return o;
}

// J^T J, J the Jacobian of r in (a, b, rho, m, sigma): dr/dtheta = (dV/dw / noise) dw/dtheta.
std::optional<Matrix5> NormalMatrix(const FitSlice& slice, const SviParams& q) {
  Matrix5 jtj{};
  for (const FitQuote& quote : slice.quotes) {
    if (!Positive(quote.strike) || !Positive(quote.noise)) return std::nullopt;
    const double k = std::log(quote.strike / slice.forward);
    const double x = k - q.m;
    const double root = std::sqrt((x * x) + (q.sigma * q.sigma));
    const double w = q.a + (q.b * ((q.rho * x) + root));
    if (!(w > 0.0)) return std::nullopt;
    const double s = std::sqrt(w);
    const double d1 = (-k + (0.5 * w)) / s;
    const double phi =
        std::exp(-0.5 * d1 * d1) * 0.5 * std::numbers::sqrt2 * std::numbers::inv_sqrtpi;
    const double dvdw = slice.discount * slice.forward * phi / (2.0 * s) / quote.noise;
    const std::array<double, 5> row = {dvdw, dvdw * ((q.rho * x) + root), dvdw * q.b * x,
                                       -dvdw * q.b * (q.rho + (x / root)),
                                       dvdw * q.b * q.sigma / root};
    for (std::size_t i = 0; i < 5; ++i) {
      for (std::size_t j = 0; j < 5; ++j) jtj[i][j] += row[i] * row[j];
    }
  }
  return jtj;
}

// One Gauss-Jordan column: pivot, normalise, eliminate from every other row. False if singular.
bool EliminateColumn(Matrix5& a, Matrix5& inv, std::size_t col) {
  std::size_t pivot = col;
  for (std::size_t row = col + 1; row < 5; ++row) {
    if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
  }
  if (!(std::abs(a[pivot][col]) > 1e-15)) return false;
  std::swap(a[col], a[pivot]);
  std::swap(inv[col], inv[pivot]);
  const double d = a[col][col];
  for (std::size_t j = 0; j < 5; ++j) {
    a[col][j] /= d;
    inv[col][j] /= d;
  }
  for (std::size_t row = 0; row < 5; ++row) {
    if (row == col) continue;
    const double f = a[row][col];
    for (std::size_t j = 0; j < 5; ++j) {
      a[row][j] -= f * a[col][j];
      inv[row][j] -= f * inv[col][j];
    }
  }
  return true;
}

// Gauss-Jordan with partial pivoting on the Jacobi-scaled matrix.
std::optional<Matrix5> Invert(const Matrix5& jtj) {
  std::array<double, 5> scale{};
  for (std::size_t i = 0; i < 5; ++i) {
    if (!(jtj[i][i] > 0.0) || !std::isfinite(jtj[i][i])) return std::nullopt;
    scale[i] = 1.0 / std::sqrt(jtj[i][i]);
  }
  Matrix5 a{};
  Matrix5 inv{};
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t j = 0; j < 5; ++j) a[i][j] = jtj[i][j] * scale[i] * scale[j];
    inv[i][i] = 1.0;
  }
  for (std::size_t col = 0; col < 5; ++col) {
    if (!EliminateColumn(a, inv, col)) return std::nullopt;
  }
  for (std::size_t i = 0; i < 5; ++i) {
    for (std::size_t j = 0; j < 5; ++j) inv[i][j] *= scale[i] * scale[j];
  }
  return inv;
}

}  // namespace

const char* BoundName(Bound bound) noexcept {
  switch (bound) {
    case Bound::kRhoLower:
      return "rho=-1";
    case Bound::kRhoUpper:
      return "rho=+1";
    case Bound::kBZero:
      return "b=0";
    case Bound::kSlope:
      return "b(1+|rho|)=bound";
    case Bound::kAUpper:
      return "a=a_max";
    case Bound::kMLower:
      return "m=m_lo";
    case Bound::kMUpper:
      return "m=m_hi";
    case Bound::kSigmaLower:
      return "sigma=sigma_lo";
    case Bound::kSigmaUpper:
      return "sigma=sigma_hi";
    case Bound::kMinVariance:
      return "min_w=0";
  }
  return "unknown";
}

const char* FitErrorName(FitErrorCode code) noexcept {
  switch (code) {
    case FitErrorCode::kBadSlice:
      return "bad-slice";
    case FitErrorCode::kBadQuote:
      return "bad-quote";
    case FitErrorCode::kTooFewStrikes:
      return "too-few-strikes";
    case FitErrorCode::kNoImpliedVol:
      return "no-implied-vol";
    case FitErrorCode::kNoFeasibleStart:
      return "no-feasible-start";
  }
  return "unknown";
}

std::vector<Bound> FitResult::ActiveBounds() const {
  std::vector<Bound> out;
  for (const BoundState& b : bounds) {
    if (b.active) out.push_back(b.bound);
  }
  return out;
}

std::expected<FitResult, FitError> FitRawSvi(const FitSlice& slice, const FitOptions& raw_options) {
  const FitOptions options = Sanitised(raw_options);
  auto prepared = Prepare(slice);
  if (!prepared) return std::unexpected(prepared.error());
  const Prepared& p = *prepared;

  const double width = p.k_max - p.k_min;
  Profile profile{.p = &p,
                  .options = &options,
                  .m_lo = p.k_min - width,
                  .m_hi = p.k_max + width,
                  .u_lo = std::log(options.sigma_lo),
                  .u_hi = std::log(options.sigma_hi),
                  .evaluations = 0,
                  .co = {}};

  // The grid, in index order, so ties go to the first.
  const double hm = width / static_cast<double>(options.grid_m - 1);
  const double g_lo = std::log(options.grid_sigma_lo);
  const double hu =
      (std::log(options.grid_sigma_hi) - g_lo) / static_cast<double>(options.grid_sigma - 1);
  std::vector<Vertex> grid;
  grid.reserve(options.grid_m * options.grid_sigma);
  for (std::size_t i = 0; i < options.grid_m; ++i) {
    for (std::size_t j = 0; j < options.grid_sigma; ++j) {
      const double m = p.k_min + (hm * static_cast<double>(i));
      const double u = std::clamp(g_lo + (hu * static_cast<double>(j)), profile.u_lo, profile.u_hi);
      grid.push_back({.m = m, .u = u, .f = profile(m, u)});
    }
  }
  std::ranges::stable_sort(grid, {}, &Vertex::f);
  if (!std::isfinite(grid.front().f)) {
    return std::unexpected(FitError{.code = FitErrorCode::kNoFeasibleStart});
  }

  std::vector<StartResult> results;
  for (std::size_t s = 0; s < options.starts && std::isfinite(grid[s].f); ++s) {
    results.push_back(NelderMead(profile, grid[s], hm, hu));
  }
  const auto best_it = std::ranges::min_element(
      results, [](const StartResult& x, const StartResult& y) { return x.best.f < y.best.f; });
  const StartResult& best = *best_it;

  FitResult r;
  r.m_lo = profile.m_lo;
  r.m_hi = profile.m_hi;
  r.sigma_lo = options.sigma_lo;
  r.sigma_hi = options.sigma_hi;
  r.a_max = p.w_max;
  r.starts = results.size();
  r.starts_at_best =
      static_cast<std::size_t>(std::ranges::count_if(results, [&](const StartResult& x) {
        return x.best.f <= best.best.f * (1.0 + kStartsAtBest);
      }));
  const double sigma = std::exp(best.best.u);
  Coordinates co;
  SetCoordinates(p, best.best.m, sigma, co);
  const InnerResult inner = SolveInner(p, co, options);
  r.profile_evaluations = profile.evaluations + 1;
  r.params = ToRaw(inner.x, best.best.m, sigma);
  r.objective = inner.phi;
  r.converged = best.converged && inner.converged;
  r.quotes = p.k.size();
  FillBounds(r, options);

  const std::size_t n = p.k.size();
  r.market_vol = p.market_vol;
  r.model_vol.resize(n);
  r.model_price.resize(n);
  r.residual.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double w = inner.x[0] + (inner.x[1] * co.y[i]) + (inner.x[2] * co.z[i]);
    r.model_vol[i] = std::sqrt(w / p.expiry);
    r.model_price[i] = ModelPrice(p, i, w);
    r.residual[i] = (r.model_price[i] - p.price[i]) * p.inv_noise[i];
  }
  return r;
}

double Objective(const FitSlice& slice, const SviParams& params) {
  if (!Positive(slice.forward) || !Positive(slice.discount) || !Positive(slice.expiry)) return kNaN;
  double sum = 0.0;
  for (const FitQuote& q : slice.quotes) {
    if (!Positive(q.strike) || !Positive(q.noise) || !std::isfinite(q.price)) return kNaN;
    const double w = TotalVariance(params, std::log(q.strike / slice.forward));
    if (!(w > 0.0)) return kNaN;
    const double v = Black76Price(slice.forward, slice.discount, q.strike, slice.expiry,
                                  std::sqrt(w / slice.expiry), q.type);
    const double r = (v - q.price) / q.noise;
    sum += r * r;
  }
  return sum;
}

std::optional<Matrix5> ParameterCovariance(const FitSlice& slice, const SviParams& params) {
  if (!Positive(slice.forward) || !Positive(slice.discount) || !Positive(slice.expiry)) {
    return std::nullopt;
  }
  const std::optional<Matrix5> jtj = NormalMatrix(slice, params);
  if (!jtj) return std::nullopt;
  return Invert(*jtj);
}

double MeanAbsPriceErrorBp(const FitSlice& slice, std::span<const double> model_price) {
  if (model_price.size() != slice.quotes.size() || model_price.empty()) return kNaN;
  double sum = 0.0;
  for (std::size_t i = 0; i < model_price.size(); ++i) {
    sum += std::abs(model_price[i] - slice.quotes[i].price);
  }
  return 1e4 * sum / static_cast<double>(model_price.size()) / slice.forward;
}

double ImpliedVolRmse(std::span<const double> model_vol, std::span<const double> market_vol) {
  if (model_vol.size() != market_vol.size() || model_vol.empty()) return kNaN;
  double sum = 0.0;
  for (std::size_t i = 0; i < model_vol.size(); ++i) {
    const double e = model_vol[i] - market_vol[i];
    sum += e * e;
  }
  return 100.0 * std::sqrt(sum / static_cast<double>(model_vol.size()));
}

}  // namespace aadp::svi
