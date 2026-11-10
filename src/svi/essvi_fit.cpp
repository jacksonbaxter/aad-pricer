#include "svi/essvi_fit.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "ad/dual.hpp"
#include "rng/philox.hpp"
#include "svi/arbitrage.hpp"
#include "svi/essvi.hpp"
#include "svi/fit.hpp"
#include "svi/implied_vol.hpp"

namespace aadp::svi {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t kMinStrikes = 5;
constexpr double kStartsAtBest = 1e-8;
constexpr double kLambdaFloor = 1e-12;
constexpr double kMinTotalVariance = 0x1p-1000;

bool Positive(double x) { return std::isfinite(x) && x > 0.0; }

// One slice, checked and inverted once, as stage 6b's fit prepares it.
struct Prepared {
  double forward = 0.0;
  double discount = 0.0;
  double expiry = 0.0;
  std::vector<double> k;
  std::vector<double> strike;
  std::vector<double> price;
  std::vector<double> inv_noise;
  std::vector<OptionType> type;
  std::vector<double> market_vol;
  double w_min = 0.0;
  double w_max = 0.0;
  double w_atm = 0.0;  // the market's total variance at k = 0, linear in k between quotes
};

// The market's total variance at k = 0: below and above it, the nearest quotes; between them,
// linear in k. k and w are not empty.
double AtTheMoney(std::span<const double> k, std::span<const double> w) {
  const std::size_t n = k.size();
  std::size_t below = n;
  std::size_t above = n;
  for (std::size_t i = 0; i < n; ++i) {
    if (k[i] <= 0.0 && (below == n || k[i] > k[below])) below = i;
    if (k[i] >= 0.0 && (above == n || k[i] < k[above])) above = i;
  }
  if (below == n) return w[above];
  if (above == n || k[above] == k[below]) return w[below];
  const double t = -k[below] / (k[above] - k[below]);
  return w[below] + (t * (w[above] - w[below]));
}

std::expected<Prepared, FitError> Prepare(const FitSlice& slice) {
  if (!Positive(slice.forward) || !Positive(slice.discount) || !Positive(slice.expiry)) {
    return std::unexpected(FitError{.code = FitErrorCode::kBadSlice});
  }
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
  Prepared p;
  p.forward = slice.forward;
  p.discount = slice.discount;
  p.expiry = slice.expiry;
  std::vector<double> w(n);
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
    w[i] = iv.vol * iv.vol * slice.expiry;
    // A total variance below 2^-1000 is refused with the non-positive ones: theta_1's box would
    // reach the subnormals, where the constraints cannot be held to rounding (NOTES.md, "Stage 7c
    // results", decisions after the pre-registration).
    if (!std::isfinite(kk) || !(w[i] >= kMinTotalVariance)) {
      return std::unexpected(FitError{.code = FitErrorCode::kNoImpliedVol,
                                      .quote = i,
                                      .status = InversionStatus::kNotAttained});
    }
    p.k.push_back(kk);
    p.strike.push_back(q.strike);
    p.price.push_back(q.price);
    p.inv_noise.push_back(1.0 / q.noise);
    p.type.push_back(q.type);
    p.market_vol.push_back(iv.vol);
  }
  p.w_min = *std::ranges::min_element(w);
  p.w_max = *std::ranges::max_element(w);
  p.w_atm = AtTheMoney(p.k, w);
  return p;
}

double ModelPrice(const Prepared& p, std::size_t i, double w) {
  return Black76Price(p.forward, p.discount, p.strike[i], p.expiry, std::sqrt(w / p.expiry),
                      p.type[i]);
}

// dV/dw = D F phi(d1) / (2 sqrt(w)) for calls and puts alike.
double PriceSlope(const Prepared& p, std::size_t i, double w) {
  const double s = std::sqrt(w);
  const double d1 = (-p.k[i] + (0.5 * w)) / s;
  const double phi =
      std::exp(-0.5 * d1 * d1) * 0.5 * std::numbers::sqrt2 * std::numbers::inv_sqrtpi;
  return p.discount * p.forward * phi / (2.0 * s);
}

struct Problem {
  std::vector<Prepared> slices;
  std::size_t quotes = 0;
  std::vector<double> lower;
  std::vector<double> upper;
};

std::size_t Coordinates(const Problem& pb) { return 3 * pb.slices.size(); }

// r and dV/dw per quote, slice after slice; Phi, or infinity where a w is not positive.
double Residuals(const Problem& pb, std::span<const double> x, std::vector<double>& r,
                 std::vector<double>& slope) {
  const std::vector<EssviSlice<double>> s = FromGlobal<double>(x);
  double phi = 0.0;
  std::size_t row = 0;
  for (std::size_t j = 0; j < pb.slices.size(); ++j) {
    const Prepared& p = pb.slices[j];
    for (std::size_t i = 0; i < p.k.size(); ++i, ++row) {
      const double w = EssviTotalVariance(s[j], p.k[i]);
      if (!Positive(w)) return kInf;
      const double v = ModelPrice(p, i, w);
      if (!std::isfinite(v)) return kInf;
      r[row] = (v - p.price[i]) * p.inv_noise[i];
      slope[row] = PriceSlope(p, i, w) * p.inv_noise[i];
      phi += r[row] * r[row];
    }
  }
  return std::isfinite(phi) ? phi : kInf;
}

// J, quotes x coordinates, row-major: dr/dx = (dV/dw / noise) dw/dx, dw/dx by a Dual through the
// map and the slice, one coordinate at a time.
void Jacobian(const Problem& pb, std::span<const double> x, std::span<const double> slope,
              std::vector<double>& jac) {
  using D = ad::Dual<double, 1>;
  const std::size_t nc = Coordinates(pb);
  std::vector<D> xd(x.begin(), x.end());
  for (std::size_t c = 0; c < nc; ++c) {
    if (c > 0) xd[c - 1].tangent[0] = 0.0;
    xd[c].tangent[0] = 1.0;
    const std::vector<EssviSlice<D>> s = FromGlobal<D>(std::span<const D>(xd));
    std::size_t row = 0;
    for (std::size_t j = 0; j < pb.slices.size(); ++j) {
      const Prepared& p = pb.slices[j];
      for (std::size_t i = 0; i < p.k.size(); ++i, ++row) {
        const D w = EssviTotalVariance(s[j], D(p.k[i]));
        jac[(row * nc) + c] = slope[row] * w.tangent[0];
      }
    }
  }
}

// The lower Cholesky factor l of (A + lambda diag A) on the coordinates idx, A being n x n; false
// if that is not positive definite.
bool Cholesky(std::span<const double> a, std::size_t n, std::span<const std::size_t> idx,
              double lambda, std::vector<double>& l) {
  const std::size_t m = idx.size();
  l.assign(m * m, 0.0);
  for (std::size_t r = 0; r < m; ++r) {
    for (std::size_t c = 0; c <= r; ++c) {
      double v = a[(idx[r] * n) + idx[c]];
      if (r == c) v *= 1.0 + lambda;
      for (std::size_t q = 0; q < c; ++q) v -= l[(r * m) + q] * l[(c * m) + q];
      if (r == c) {
        if (!(v > 0.0) || !std::isfinite(v)) return false;
        l[(r * m) + r] = std::sqrt(v);
      } else {
        l[(r * m) + c] = v / l[(c * m) + c];
      }
    }
  }
  return true;
}

// Solves (A + lambda diag A) d = -g on the free coordinates by Cholesky; false if not positive
// definite. The others get d = 0.
bool DampedStep(std::span<const double> a, std::span<const double> g,
                std::span<const std::uint8_t> free, double lambda, std::vector<double>& d) {
  const std::size_t n = g.size();
  std::vector<std::size_t> idx;
  for (std::size_t j = 0; j < n; ++j) {
    if (free[j] != 0) idx.push_back(j);
  }
  const std::size_t m = idx.size();
  std::vector<double> l;
  if (!Cholesky(a, n, idx, lambda, l)) return false;
  std::vector<double> y(m);
  for (std::size_t r = 0; r < m; ++r) {
    double v = -g[idx[r]];
    for (std::size_t q = 0; q < r; ++q) v -= l[(r * m) + q] * y[q];
    y[r] = v / l[(r * m) + r];
  }
  d.assign(n, 0.0);
  for (std::size_t r = m; r-- > 0;) {
    double v = y[r];
    for (std::size_t q = r + 1; q < m; ++q) v -= l[(q * m) + r] * d[idx[q]];
    d[idx[r]] = v / l[(r * m) + r];
  }
  return m > 0;
}

struct Minimum {
  std::vector<double> x;
  double phi = kInf;
  EssviStop stop = EssviStop::kIterations;
  std::size_t iterations = 0;
  double gradient = kNaN;
};

class Minimiser {
 public:
  Minimiser(const Problem& pb, const EssviFitOptions& o)
      : pb_(&pb),
        o_(o),
        nc_(Coordinates(pb)),
        r_(pb.quotes),
        slope_(pb.quotes),
        jac_(pb.quotes * nc_),
        a_(nc_ * nc_),
        g_(nc_),
        free_(nc_),
        trial_(nc_),
        r_trial_(pb.quotes),
        slope_trial_(pb.quotes) {}

  Minimum Run(std::vector<double> x) {
    for (std::size_t j = 0; j < nc_; ++j) x[j] = std::clamp(x[j], pb_->lower[j], pb_->upper[j]);
    Minimum out;
    double phi = Residuals(*pb_, x, r_, slope_);
    if (!std::isfinite(phi)) {
      out.x = std::move(x);
      return out;
    }
    double lambda = o_.lambda_start;
    std::size_t slow = 0;
    std::size_t it = 0;
    bool stopped = false;
    for (; it < o_.max_iterations && !stopped; ++it) {
      out.gradient = Normal(x, phi);
      if (out.gradient <= o_.gradient_tolerance) {
        out.stop = EssviStop::kStationary;
        break;
      }
      double next = phi;
      if (!Search(x, phi, lambda, next)) {
        out.stop = EssviStop::kNoDecrease;
        break;
      }
      slow = (phi - next) <= o_.slow_decrease * phi ? slow + 1 : 0;
      x.swap(trial_);
      r_.swap(r_trial_);
      slope_.swap(slope_trial_);
      phi = next;
      lambda = std::max(lambda / 3.0, kLambdaFloor);
      if (slow >= o_.slow_steps) {
        out.stop = EssviStop::kSlow;
        stopped = true;
      }
    }
    if (out.stop == EssviStop::kSlow || out.stop == EssviStop::kIterations) {
      out.gradient = Normal(x, phi);
    }
    out.iterations = it;
    out.x = std::move(x);
    out.phi = phi;
    return out;
  }

 private:
  // Raises lambda from its value until a damped step from x lowers Phi, or lambda passes
  // lambda_max; true with the trial point, its residuals and its Phi (next) if one did.
  bool Search(const std::vector<double>& x, double phi, double& lambda, double& next) {
    while (lambda <= o_.lambda_max) {
      if (DampedStep(a_, g_, free_, lambda, d_)) {
        for (std::size_t j = 0; j < nc_; ++j) {
          trial_[j] = std::clamp(x[j] + d_[j], pb_->lower[j], pb_->upper[j]);
        }
        next = trial_ != x ? Residuals(*pb_, trial_, r_trial_, slope_trial_) : kInf;
        if (next < phi) return true;
      }
      lambda *= 4.0;
    }
    return false;
  }

  // A = J^T J, g = J^T r and the free set at x; returns the scaled projected gradient.
  double Normal(std::span<const double> x, double phi) {
    Jacobian(*pb_, x, slope_, jac_);
    std::ranges::fill(a_, 0.0);
    std::ranges::fill(g_, 0.0);
    for (std::size_t q = 0; q < pb_->quotes; ++q) {
      const std::span<const double> row(jac_.data() + (q * nc_), nc_);
      for (std::size_t i = 0; i < nc_; ++i) {
        g_[i] += row[i] * r_[q];
        for (std::size_t j = 0; j <= i; ++j) a_[(i * nc_) + j] += row[i] * row[j];
      }
    }
    for (std::size_t i = 0; i < nc_; ++i) {
      for (std::size_t j = 0; j < i; ++j) a_[(j * nc_) + i] = a_[(i * nc_) + j];
    }
    double worst = 0.0;
    for (std::size_t j = 0; j < nc_; ++j) {
      const bool out_low = x[j] <= pb_->lower[j] && g_[j] > 0.0;
      const bool out_high = x[j] >= pb_->upper[j] && g_[j] < 0.0;
      const double ajj = a_[(j * nc_) + j];
      const bool free = !out_low && !out_high && ajj > 0.0 && std::isfinite(ajj);
      free_[j] = free ? 1 : 0;
      if (free && phi > 0.0) worst = std::max(worst, std::fabs(g_[j]) / std::sqrt(ajj * phi));
    }
    return worst;
  }

  const Problem* pb_;
  EssviFitOptions o_;
  std::size_t nc_;
  std::vector<double> r_;
  std::vector<double> slope_;
  std::vector<double> jac_;
  std::vector<double> a_;
  std::vector<double> g_;
  std::vector<std::uint8_t> free_;
  std::vector<double> d_;
  std::vector<double> trial_;
  std::vector<double> r_trial_;
  std::vector<double> slope_trial_;
};

// The starts: Mingone's, then uniform draws in a stated part of the box.
std::vector<std::vector<double>> Starts(const Problem& pb, const EssviFitOptions& o) {
  const std::size_t n = pb.slices.size();
  std::vector<std::vector<double>> out;
  std::vector<double> x(3 * n, 0.0);
  x[n] = pb.slices[0].w_atm;
  for (std::size_t i = 1; i < n; ++i) {
    x[n + i] = std::max(pb.slices[i].w_atm - pb.slices[i - 1].w_atm, 0.0);
  }
  for (std::size_t i = 0; i < n; ++i) x[(2 * n) + i] = 0.5;
  out.push_back(x);
  for (std::size_t s = 1; s <= o.random_starts; ++s) {
    const auto u = [&](std::size_t coordinate) {
      return PathUniform(o.seed, s, static_cast<std::uint32_t>(coordinate));
    };
    for (std::size_t i = 0; i < n; ++i) x[i] = -0.95 + (1.9 * u(i));
    x[n] = pb.slices[0].w_atm * (0.5 + u(n));
    for (std::size_t i = 1; i < n; ++i) {
      const double step =
          std::max(pb.slices[i].w_atm - pb.slices[i - 1].w_atm, 0.1 * pb.slices[i].w_atm);
      x[n + i] = 2.0 * step * u(n + i);
    }
    for (std::size_t i = 0; i < n; ++i) x[(2 * n) + i] = 0.05 + (0.9 * u((2 * n) + i));
    out.push_back(x);
  }
  return out;
}

std::expected<Problem, EssviFitError> MakeProblem(std::span<const FitSlice> slices) {
  const std::size_t n = slices.size();
  if (n == 0 || n > kMaxEssviSlices) {
    return std::unexpected(
        EssviFitError{.code = EssviFitErrorCode::kSliceCount, .slice = 0, .fit = {}});
  }
  Problem pb;
  for (std::size_t j = 0; j < n; ++j) {
    auto p = Prepare(slices[j]);
    if (!p) {
      return std::unexpected(
          EssviFitError{.code = EssviFitErrorCode::kSlice, .slice = j, .fit = p.error()});
    }
    if (j > 0 && !(p->expiry > pb.slices.back().expiry)) {
      return std::unexpected(
          EssviFitError{.code = EssviFitErrorCode::kExpiryOrder, .slice = j, .fit = {}});
    }
    pb.quotes += p->k.size();
    pb.slices.push_back(std::move(*p));
  }
  pb.lower.assign(3 * n, 0.0);
  pb.upper.assign(3 * n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    pb.lower[i] = -kRhoBound;
    pb.upper[i] = kRhoBound;
    pb.lower[(2 * n) + i] = i == 0 ? kC1Lower : 0.0;
    pb.upper[(2 * n) + i] = 1.0;
    if (i > 0) pb.upper[n + i] = 10.0 * pb.slices[i].w_max;
  }
  pb.lower[n] = 1e-3 * pb.slices[0].w_min;
  pb.upper[n] = 10.0 * pb.slices[0].w_max;
  return pb;
}

// One column of Gauss-Jordan with partial pivoting on m, applied to inv as well; false if the
// pivot is below 1e-14 (m is scaled to a unit diagonal).
bool Eliminate(std::vector<double>& m, std::vector<double>& inv, std::size_t np, std::size_t c) {
  std::size_t piv = c;
  for (std::size_t r = c + 1; r < np; ++r) {
    if (std::fabs(m[(r * np) + c]) > std::fabs(m[(piv * np) + c])) piv = r;
  }
  if (!(std::fabs(m[(piv * np) + c]) > 1e-14)) return false;
  for (std::size_t k = 0; k < np; ++k) {
    std::swap(m[(c * np) + k], m[(piv * np) + k]);
    std::swap(inv[(c * np) + k], inv[(piv * np) + k]);
  }
  const double d = m[(c * np) + c];
  for (std::size_t k = 0; k < np; ++k) {
    m[(c * np) + k] /= d;
    inv[(c * np) + k] /= d;
  }
  for (std::size_t r = 0; r < np; ++r) {
    if (r == c) continue;
    const double f = m[(r * np) + c];
    if (f == 0.0) continue;
    for (std::size_t k = 0; k < np; ++k) {
      m[(r * np) + k] -= f * m[(c * np) + k];
      inv[(r * np) + k] -= f * inv[(c * np) + k];
    }
  }
  return true;
}

// The inverse of a (np x np), by Gauss-Jordan with partial pivoting on the matrix scaled to a unit
// diagonal; empty if a diagonal entry is not positive or a pivot is too small.
std::vector<double> InvertScaled(std::span<const double> a, std::size_t np) {
  std::vector<double> scale(np);
  for (std::size_t i = 0; i < np; ++i) {
    const double d = a[(i * np) + i];
    if (!(d > 0.0) || !std::isfinite(d)) return {};
    scale[i] = 1.0 / std::sqrt(d);
  }
  std::vector<double> m(np * np);
  std::vector<double> inv(np * np, 0.0);
  for (std::size_t i = 0; i < np; ++i) {
    inv[(i * np) + i] = 1.0;
    for (std::size_t k = 0; k < np; ++k) m[(i * np) + k] = a[(i * np) + k] * scale[i] * scale[k];
  }
  for (std::size_t c = 0; c < np; ++c) {
    if (!Eliminate(m, inv, np, c)) return {};
  }
  for (std::size_t i = 0; i < np; ++i) {
    for (std::size_t k = 0; k < np; ++k) inv[(i * np) + k] *= scale[i] * scale[k];
  }
  return inv;
}

std::size_t SliceOf(std::size_t j, std::size_t n) {
  if (j < n) return j;
  if (j < 2 * n) return j - n;
  return j - (2 * n);
}

EssviCoordinate CoordinateOf(std::size_t j, std::size_t n) {
  if (j < n) return EssviCoordinate::kRho;
  if (j == n) return EssviCoordinate::kTheta1;
  if (j < 2 * n) return EssviCoordinate::kA;
  return EssviCoordinate::kC;
}

}  // namespace

const char* EssviStopName(EssviStop stop) noexcept {
  switch (stop) {
    case EssviStop::kStationary:
      return "stationary";
    case EssviStop::kNoDecrease:
      return "no-decrease";
    case EssviStop::kSlow:
      return "slow";
    case EssviStop::kIterations:
      return "iterations";
  }
  return "unknown";
}

const char* EssviFitErrorName(EssviFitErrorCode code) noexcept {
  switch (code) {
    case EssviFitErrorCode::kSliceCount:
      return "slice-count";
    case EssviFitErrorCode::kExpiryOrder:
      return "expiry-order";
    case EssviFitErrorCode::kSlice:
      return "slice";
    case EssviFitErrorCode::kNoFeasibleStart:
      return "no-feasible-start";
  }
  return "unknown";
}

std::expected<EssviFitResult, EssviFitError> FitEssviSurface(std::span<const FitSlice> slices,
                                                             const EssviFitOptions& options) {
  auto problem = MakeProblem(slices);
  if (!problem) return std::unexpected(problem.error());
  const Problem& pb = *problem;
  const std::size_t n = pb.slices.size();

  Minimiser minimiser(pb, options);
  std::vector<Minimum> minima;
  for (const std::vector<double>& start : Starts(pb, options)) {
    minima.push_back(minimiser.Run(start));
  }
  std::size_t best = minima.size();
  for (std::size_t s = 0; s < minima.size(); ++s) {
    if (std::isfinite(minima[s].phi) &&
        (best == minima.size() || minima[s].phi < minima[best].phi)) {
      best = s;
    }
  }
  if (best == minima.size()) {
    return std::unexpected(
        EssviFitError{.code = EssviFitErrorCode::kNoFeasibleStart, .slice = 0, .fit = {}});
  }
  const Minimum& m = minima[best];

  EssviFitResult out;
  out.x = m.x;
  out.objective = m.phi;
  out.stop = m.stop;
  out.converged = m.stop != EssviStop::kIterations;
  out.iterations = m.iterations;
  out.gradient = m.gradient;
  for (const Minimum& s : minima) {
    out.starts.push_back(
        {.objective = s.phi, .stop = s.stop, .iterations = s.iterations, .gradient = s.gradient});
    if (s.phi <= m.phi * (1.0 + kStartsAtBest)) ++out.starts_at_best;
  }
  for (std::size_t j = 0; j < 3 * n; ++j) {
    const double width = pb.upper[j] - pb.lower[j];
    out.coordinates.push_back({.coordinate = CoordinateOf(j, n),
                               .slice = SliceOf(j, n),
                               .value = m.x[j],
                               .lower = pb.lower[j],
                               .upper = pb.upper[j],
                               .at_lower = m.x[j] - pb.lower[j] <= kActiveTolerance * width,
                               .at_upper = pb.upper[j] - m.x[j] <= kActiveTolerance * width});
  }
  std::vector<PsiInterval> intervals;
  const std::vector<EssviSlice<double>> fitted = FromGlobal<double>(m.x, &intervals);
  out.constraints = CheckEssviConstraints(fitted);
  for (std::size_t j = 0; j < n; ++j) {
    const Prepared& p = pb.slices[j];
    EssviSliceFit s;
    s.params = fitted[j];
    s.raw = ToRawSvi(fitted[j]);
    s.interval = intervals[j];
    s.quotes = p.k.size();
    s.market_vol = p.market_vol;
    for (std::size_t i = 0; i < p.k.size(); ++i) {
      const double w = EssviTotalVariance(fitted[j], p.k[i]);
      const double v = ModelPrice(p, i, w);
      const double r = (v - p.price[i]) * p.inv_noise[i];
      s.model_vol.push_back(std::sqrt(w / p.expiry));
      s.model_price.push_back(v);
      s.residual.push_back(r);
      s.objective += r * r;
    }
    out.slices.push_back(std::move(s));
  }
  return out;
}

double EssviObjective(std::span<const FitSlice> slices, std::span<const double> x) {
  auto problem = MakeProblem(slices);
  if (!problem || x.size() != 3 * slices.size()) return kNaN;
  std::vector<double> r(problem->quotes);
  std::vector<double> slope(problem->quotes);
  const double phi = Residuals(*problem, x, r, slope);
  return std::isfinite(phi) ? phi : kNaN;
}

std::vector<double> EssviParameterCovariance(std::span<const FitSlice> slices,
                                             std::span<const EssviSlice<double>> params) {
  auto problem = MakeProblem(slices);
  if (!problem || params.size() != slices.size()) return {};
  const Problem& pb = *problem;
  const std::size_t np = 3 * params.size();
  using D = ad::Dual<double, 3>;
  std::vector<double> a(np * np, 0.0);
  for (std::size_t j = 0; j < params.size(); ++j) {
    const Prepared& p = pb.slices[j];
    EssviSlice<D> s{.theta = params[j].theta, .rho = params[j].rho, .psi = params[j].psi};
    s.theta.tangent[0] = 1.0;
    s.rho.tangent[1] = 1.0;
    s.psi.tangent[2] = 1.0;
    for (std::size_t i = 0; i < p.k.size(); ++i) {
      const D w = EssviTotalVariance(s, D(p.k[i]));
      if (!Positive(w.value)) return {};
      const double slope = PriceSlope(p, i, w.value) * p.inv_noise[i];
      for (std::size_t u = 0; u < 3; ++u) {
        for (std::size_t v = 0; v < 3; ++v) {
          a[(((3 * j) + u) * np) + (3 * j) + v] += slope * slope * w.tangent[u] * w.tangent[v];
        }
      }
    }
  }
  return InvertScaled(a, np);
}

}  // namespace aadp::svi
