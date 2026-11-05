#include "svi/arbitrage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "svi/raw_svi.hpp"

namespace aadp::svi {

namespace {

constexpr double kEps = 0x1p-52;
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// Part of a grid with more points than this is dropped: a GridSpec that asks for more is a bug.
constexpr double kMaxGridPart = 1e7;
// Root brackets for the classifier grow by doubling out to |l| = 1e15; past that it is undecided.
constexpr double kMaxBracket = 1e15;
constexpr int kBisections = 400;
constexpr int kThresholdBisections = 120;
constexpr int kGoldenIterations = 120;

// The sum of the magnitudes of w's terms at k: w's rounding scale.
double WMagnitude(const SviParams& p, double k) {
  const double x = k - p.m;
  return std::fabs(p.a) + (p.b * (std::fabs(p.rho * x) + std::sqrt((x * x) + (p.sigma * p.sigma))));
}

double DensityToleranceAt(const SviParams& p, const SviPoint<double>& s, double k) {
  const double c_w = WMagnitude(p, k) / s.w;
  const double u = 1.0 + std::fabs(k * s.w1 / (2.0 * s.w));
  const double magnitude =
      (u * u) + (0.25 * s.w1 * s.w1 * ((1.0 / s.w) + 0.25)) + (0.5 * std::fabs(s.w2));
  return 32.0 * kEps * c_w * magnitude;
}

Violation Point(ViolationKind kind, double k, double value, double bound) {
  return {.kind = kind,
          .k_begin = k,
          .k_end = k,
          .k_worst = k,
          .value = value,
          .bound = bound,
          .points = 0};
}

// Collects runs of consecutive failing grid points into one violation each, keeping the worst.
class RunCollector {
 public:
  RunCollector(ViolationKind kind, std::vector<Violation>* out) : kind_(kind), out_(out) {}

  void Fail(double k, double value, double bound) {
    if (!open_) {
      run_ = {.kind = kind_,
              .k_begin = k,
              .k_end = k,
              .k_worst = k,
              .value = value,
              .bound = bound,
              .points = 1};
      open_ = true;
      return;
    }
    run_.k_end = k;
    ++run_.points;
    if (value < run_.value) {
      run_.value = value;
      run_.k_worst = k;
      run_.bound = bound;
    }
  }

  void Pass() {
    if (open_) out_->push_back(run_);
    open_ = false;
  }

 private:
  ViolationKind kind_;
  std::vector<Violation>* out_;
  Violation run_{};
  bool open_ = false;
};

struct DensityScan {
  std::vector<Violation> violations;
  double min_g = kInf;
  double k_min_g = kNaN;
};

DensityScan ScanDensity(const SviParams& p, const std::vector<double>& grid) {
  DensityScan scan;
  RunCollector runs(ViolationKind::kDensity, &scan.violations);
  for (const double k : grid) {
    const SviPoint<double> s = Evaluate(p, k);
    const double g = Durrleman(s, k);
    if (!std::isfinite(g)) {
      runs.Pass();
      scan.violations.push_back(Point(ViolationKind::kNonFiniteDensity, k, g, 0.0));
      scan.violations.back().points = 1;
      continue;
    }
    if (g < scan.min_g) {
      scan.min_g = g;
      scan.k_min_g = k;
    }
    const double tolerance = DensityToleranceAt(p, s, k);
    if (g < -tolerance) {
      runs.Fail(k, g, -tolerance);
    } else {
      runs.Pass();
    }
  }
  runs.Pass();
  if (grid.empty()) scan.min_g = kNaN;
  return scan;
}

// ---- Martini and Mingone, arXiv 2005.03340 ----------------------------------------------------

// rho r + l and rho l + r with r = sqrt(l^2 + 1), by the conjugate where the two terms cancel.
double RhoRPlusL(double rho, double l, double r) {
  if (rho * l < 0.0) {
    const double one_minus_rho2 = (1.0 - rho) * (1.0 + rho);
    return ((rho * rho) - (one_minus_rho2 * l * l)) / ((rho * r) - l);
  }
  return (rho * r) + l;
}

double RhoLPlusR(double rho, double l, double r) {
  if (rho * l < 0.0) {
    const double one_minus_rho2 = (1.0 - rho) * (1.0 + rho);
    return (1.0 + (one_minus_rho2 * l * l)) / (r - (rho * l));
  }
  return (rho * l) + r;
}

struct Normalised {
  double alpha;
  double b;
  double rho;
};

// N, N', N'' of MM Lemma 4.1, with u = rho r + l and v = rho l + r kept for eq. (6).
struct NAt {
  double n;
  double n1;
  double n2;
  double u;
  double v;
  double r;
};

NAt EvaluateN(const Normalised& s, double l) {
  const double r = std::hypot(l, 1.0);
  const double u = RhoRPlusL(s.rho, l, r);
  const double v = RhoLPlusR(s.rho, l, r);
  return {
      .n = s.alpha + (s.b * v), .n1 = s.b * u / r, .n2 = s.b / (r * r * r), .u = u, .v = v, .r = r};
}

// MM eq. (6): g-(l) = u^2 (r/2 + b u/4) - v. L-'(l) = 0 if and only if g-(l) = alpha / b.
double GMinus(const Normalised& s, double l) {
  const double r = std::hypot(l, 1.0);
  const double u = RhoRPlusL(s.rho, l, r);
  return (u * u * ((0.5 * r) + (0.25 * s.b * u))) - RhoLPlusR(s.rho, l, r);
}

// MM eq. (5): L-(l) = 2 N(l) (1/N'(l) + 1/4) - l, on l < l*.
double LMinus(const Normalised& s, double l) {
  const NAt n = EvaluateN(s, l);
  return (2.0 * n.n * ((1.0 / n.n1) + 0.25)) - l;
}

// MM eq. (7): G2 = N'' - N'^2 / (2 N).
double G2(const Normalised& s, double l) {
  const NAt n = EvaluateN(s, l);
  return n.n2 - (n.n1 * n.n1 / (2.0 * n.n));
}

// MM section 4.1: G1 = G1+ G1-, G1+- = 1 - N' ((l + mu) / (2 N) +- 1/4).
double G1(const Normalised& s, double mu, double l) {
  const NAt n = EvaluateN(s, l);
  const double t = (l + mu) / (2.0 * n.n);
  return (1.0 - (n.n1 * (t + 0.25))) * (1.0 - (n.n1 * (t - 0.25)));
}

struct Located {
  double value;
  double l;
  bool ok;
};

double LStar(double rho) {
  if (rho <= -1.0) return kInf;
  if (rho >= 1.0) return -kInf;
  return -rho / std::sqrt((1.0 - rho) * (1.0 + rho));
}

// sup over l < l* of L-, MM Lemma 5.2. Under b (1 - rho) = 2 (MM's cases B2 and B4) it is -alpha/2,
// at -infinity (MM section 5.2.5). Otherwise it is L-(l-) at the one root l- of g- = alpha / b
// below l* (MM Proposition 5.5 and Corollary 5.6; for rho = -1, Theorem 6.3), found by bisection
// between a point where g- > alpha / b and one where g- < alpha / b.
Located SupLMinus(const Normalised& s) {
  if (s.b * (1.0 - s.rho) == 2.0) return {.value = -0.5 * s.alpha, .l = -kInf, .ok = true};
  const double target = s.alpha / s.b;
  if (!std::isfinite(target)) return {.value = kNaN, .l = kNaN, .ok = false};
  const double l_star = LStar(s.rho);
  double hi = l_star;
  if (std::isfinite(hi)) {
    if (!(GMinus(s, hi) < target)) return {.value = kNaN, .l = kNaN, .ok = false};
  } else {
    hi = 1.0;
    while (!(GMinus(s, hi) < target)) {
      hi *= 2.0;
      if (hi > kMaxBracket) return {.value = kNaN, .l = kNaN, .ok = false};
    }
  }
  const double origin = std::min(hi, 0.0);
  double step = 1.0;
  double lo = origin - step;
  while (!(GMinus(s, lo) > target)) {
    step *= 2.0;
    if (step > kMaxBracket) return {.value = kNaN, .l = kNaN, .ok = false};
    lo = origin - step;
  }
  for (int i = 0; i < kBisections; ++i) {
    const double mid = lo + (0.5 * (hi - lo));
    if (mid <= lo || mid >= hi) break;
    if (GMinus(s, mid) > target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const double l = lo + (0.5 * (hi - lo));
  const double value = LMinus(s, l);
  return {.value = value, .l = l, .ok = std::isfinite(value)};
}

// inf over l > l* of L+, by MM Lemma 5.9: inf L+(alpha, b, rho) = -sup L-(alpha, b, -rho), at
// l+ = -l-(alpha, b, -rho).
Located InfLPlus(const Normalised& s) {
  const Located mirrored = SupLMinus({.alpha = s.alpha, .b = s.b, .rho = -s.rho});
  return {.value = -mirrored.value, .l = -mirrored.l, .ok = mirrored.ok};
}

// F(b, rho), MM section 5.2.2: the alpha above which I_{alpha,b,rho} is non-empty. The width
// inf L+ - sup L- increases with alpha (MM Corollary 5.6), and the interval is non-empty at
// alpha = 0 except in case B4 (MM Lemma 5.4), so bisection on [-b sqrt(1 - rho^2), 0].
double FukasawaThreshold(double b, double rho) {
  if (b * (1.0 + rho) == 2.0 && b * (1.0 - rho) == 2.0) return 0.0;
  const auto width = [&](double alpha, bool& ok) {
    const Normalised s{.alpha = alpha, .b = b, .rho = rho};
    const Located lower = SupLMinus(s);
    const Located upper = InfLPlus(s);
    ok = lower.ok && upper.ok;
    return upper.value - lower.value;
  };
  bool ok = false;
  if (!(width(0.0, ok) > 0.0) || !ok) return kNaN;
  double lo = -b * std::sqrt((1.0 - rho) * (1.0 + rho));
  double hi = 0.0;
  for (int i = 0; i < kThresholdBisections; ++i) {
    const double mid = lo + (0.5 * (hi - lo));
    if (mid <= lo || mid >= hi) break;
    const double w = width(mid, ok);
    if (ok && w > 0.0) {
      hi = mid;
    } else {
      lo = mid;
    }
  }
  return hi;
}

// The zero of G2 beyond start, which has G2(start) > 0 (MM Lemma 6.1), in direction -1 or +1.
Located ZeroOfG2(const Normalised& s, double start, double direction) {
  if (!(G2(s, start) > 0.0)) return {.value = kNaN, .l = kNaN, .ok = false};
  double step = 1.0;
  double far = start + (direction * step);
  while (!(G2(s, far) < 0.0)) {
    step *= 2.0;
    if (step > kMaxBracket) return {.value = kNaN, .l = kNaN, .ok = false};
    far = start + (direction * step);
  }
  double inside = start;
  for (int i = 0; i < kBisections; ++i) {
    const double mid = inside + (0.5 * (far - inside));
    if (mid == inside || mid == far) break;
    if (G2(s, mid) > 0.0) {
      inside = mid;
    } else {
      far = mid;
    }
  }
  return {.value = 0.0, .l = inside + (0.5 * (far - inside)), .ok = true};
}

// sup of f(h) = -G2(1/h) / (2 G1(1/h)) for h strictly between 0 and h_end = 1/l_zero (MM section
// 6.2.1), with l at the maximum. +infinity if G1 is not positive somewhere there, which the
// Fukasawa conditions exclude.
Located SupRatio(const Normalised& s, double mu, double h_end) {
  const auto f = [&](double h) {
    const double l = 1.0 / h;
    const double g1 = G1(s, mu, l);
    if (!(g1 > 0.0)) return kInf;
    return -G2(s, l) / (2.0 * g1);
  };
  constexpr double kSteps = static_cast<double>(kMmSigmaGrid) + 1.0;
  double best = -kInf;
  std::size_t best_j = 1;
  for (std::size_t j = 1; j <= kMmSigmaGrid; ++j) {
    const double value = f(h_end * (static_cast<double>(j) / kSteps));
    if (!(value <= kInf)) return {.value = kNaN, .l = kNaN, .ok = false};
    if (value > best) {
      best = value;
      best_j = j;
    }
  }
  if (best == kInf) return {.value = kInf, .l = kNaN, .ok = true};
  // Golden section on the two grid intervals around the best point. The point before j = 1 is
  // h = 0, which cannot be evaluated, so half of h_1 stands in for it.
  const double step = h_end / kSteps;
  double lo = best_j == 1 ? 0.5 * step : step * static_cast<double>(best_j - 1);
  double hi = step * static_cast<double>(best_j + 1);
  if (best_j == kMmSigmaGrid) hi = h_end;
  const double ratio = 0.5 * (std::sqrt(5.0) - 1.0);
  double x1 = hi - (ratio * (hi - lo));
  double x2 = lo + (ratio * (hi - lo));
  double f1 = f(x1);
  double f2 = f(x2);
  for (int i = 0; i < kGoldenIterations; ++i) {
    if (f1 < f2) {
      lo = x1;
      x1 = x2;
      f1 = f2;
      x2 = lo + (ratio * (hi - lo));
      f2 = f(x2);
    } else {
      hi = x2;
      x2 = x1;
      f2 = f1;
      x1 = hi - (ratio * (hi - lo));
      f1 = f(x1);
    }
  }
  const double h_best = f1 > f2 ? x1 : x2;
  const double refined = std::max(f1, f2);
  if (refined > best) return {.value = refined, .l = 1.0 / h_best, .ok = true};
  return {.value = best, .l = 1.0 / (step * static_cast<double>(best_j)), .ok = true};
}

struct Classified {
  MartiniMingone result;
  double k_location = kNaN;  // where the failing condition binds
  double threshold = kNaN;   // what failed: the slope, F, the interval end or sigma*
};

// k of a normalised l, undoing the reflection.
struct KOfL {
  double sigma;
  double mu;
  bool reflected;
  [[nodiscard]] double operator()(double l) const {
    const double k = sigma * (l + mu);
    return reflected ? -k : k;
  }
};

// Steps before the normalisation: parameters, b = 0 and type 1. True if they decide.
bool ClassifyTrivial(const SviParams& p, Classified& c) {
  MartiniMingone& r = c.result;
  const std::vector<Violation> parameters = CheckParameters(p);
  if (!parameters.empty()) {
    r.verdict = parameters.front().kind == ViolationKind::kInvalidParameters
                    ? MmVerdict::kInvalidParameters
                    : MmVerdict::kNonPositiveVariance;
    return true;
  }
  // MM exclude b = 0, the Black-Scholes case, as trivially free of arbitrage (section 4.2).
  if (p.b == 0.0) {
    r.verdict = MmVerdict::kNoArbitrage;
    return true;
  }
  const double right = p.b * (1.0 + p.rho);
  const double left = p.b * (1.0 - p.rho);
  if (right > 2.0 || left > 2.0) {
    r.verdict = MmVerdict::kType1;
    c.k_location = right > 2.0 ? kInf : -kInf;
    c.threshold = right > 2.0 ? right : left;
    return true;
  }
  return false;
}

// Types 2 and 3: the interval I for mu, MM Theorem 5.10 (Theorem 6.3 for rho = -1). True if they
// decide, which includes undecided.
bool ClassifyInterval(const Normalised& s, const KOfL& k_of, double b, Classified& c) {
  MartiniMingone& r = c.result;
  Located lower{};
  Located upper{};
  if (s.rho <= -1.0) {
    // MM section 6.4, Theorem 6.3: F(b, -1) = 0, and alpha >= 0 is the positivity already
    // checked, so there is no type 2. The interval for mu is ]sup L-, +infinity[.
    lower = SupLMinus(s);
    upper = {.value = kInf, .l = kInf, .ok = true};
  } else {
    r.fukasawa_threshold = FukasawaThreshold(b, s.rho);
    lower = SupLMinus(s);
    upper = InfLPlus(s);
  }
  if (!lower.ok || !upper.ok) {
    r.verdict = MmVerdict::kUndecided;
    return true;
  }
  r.mu_lower = lower.value;
  r.mu_upper = upper.value;
  if (!(lower.value < upper.value)) {
    r.verdict = MmVerdict::kType2;
    c.k_location = k_of(lower.l);
    c.threshold = r.fukasawa_threshold;
    return true;
  }
  const double mu = r.mu;
  if (!(lower.value < mu && mu < upper.value)) {
    r.verdict = MmVerdict::kType3;
    const bool below = !(lower.value < mu);
    c.k_location = k_of(below ? lower.l : upper.l);
    c.threshold = below ? lower.value : upper.value;
    return true;
  }
  return false;
}

// Step 4, MM Theorem 6.2 (Theorem 6.3 for rho = -1): sigma >= sigma*.
void ClassifySigma(const Normalised& s, const KOfL& k_of, double sigma, Classified& c) {
  MartiniMingone& r = c.result;
  const double mu = r.mu;
  const double l_star = LStar(s.rho);
  const Located z1 = ZeroOfG2(s, std::min(l_star, 0.0), -1.0);
  if (!z1.ok) {
    r.verdict = MmVerdict::kUndecided;
    return;
  }
  r.l1 = z1.l;
  Located sup = SupRatio(s, mu, 1.0 / z1.l);
  if (s.rho > -1.0) {
    const Located z2 = ZeroOfG2(s, std::max(l_star, 0.0), 1.0);
    if (!z2.ok) {
      r.verdict = MmVerdict::kUndecided;
      return;
    }
    r.l2 = z2.l;
    const Located right_sup = SupRatio(s, mu, 1.0 / z2.l);
    if (!right_sup.ok) sup.ok = false;
    if (right_sup.ok && right_sup.value > sup.value) sup = right_sup;
  }
  if (!sup.ok) {
    r.verdict = MmVerdict::kUndecided;
    return;
  }
  r.sigma_star = std::max(sup.value, 0.0);
  if (sigma < r.sigma_star) {
    r.verdict = MmVerdict::kType4;
    c.k_location = k_of(sup.l);
    c.threshold = r.sigma_star;
    return;
  }
  r.verdict = MmVerdict::kNoArbitrage;
}

Classified Classify(const SviParams& p) {
  Classified c;
  MartiniMingone& r = c.result;
  r.fukasawa_threshold = kNaN;
  r.mu_lower = kNaN;
  r.mu_upper = kNaN;
  r.l1 = kNaN;
  r.l2 = kNaN;
  r.sigma_star = kNaN;
  r.alpha = p.a / p.sigma;
  r.mu = p.m / p.sigma;
  if (ClassifyTrivial(p, c)) return c;

  double rho = p.rho;
  if (rho >= 1.0) {
    r.reflected = true;
    rho = -1.0;
    r.mu = -r.mu;
  }
  const KOfL k_of{.sigma = p.sigma, .mu = r.mu, .reflected = r.reflected};
  const Normalised s{.alpha = r.alpha, .b = p.b, .rho = rho};
  if (ClassifyInterval(s, k_of, p.b, c)) return c;
  ClassifySigma(s, k_of, p.sigma, c);
  return c;
}

double VerdictNumber(MmVerdict verdict) {
  switch (verdict) {
    case MmVerdict::kType1:
      return 1.0;
    case MmVerdict::kType2:
      return 2.0;
    case MmVerdict::kType3:
      return 3.0;
    case MmVerdict::kType4:
      return 4.0;
    default:
      return 0.0;
  }
}

}  // namespace

const char* KindName(ViolationKind kind) noexcept {
  switch (kind) {
    case ViolationKind::kInvalidParameters:
      return "invalid-parameters";
    case ViolationKind::kNonPositiveVariance:
      return "non-positive-variance";
    case ViolationKind::kDensity:
      return "density";
    case ViolationKind::kNonFiniteDensity:
      return "non-finite-density";
    case ViolationKind::kDPlusLimit:
      return "d-plus-limit";
    case ViolationKind::kRightWing:
      return "right-wing";
    case ViolationKind::kLeftWing:
      return "left-wing";
    case ViolationKind::kMartiniMingone:
      return "martini-mingone";
    case ViolationKind::kCalendarCrossing:
      return "calendar-crossing";
    case ViolationKind::kCalendarRightWing:
      return "calendar-right-wing";
    case ViolationKind::kCalendarLeftWing:
      return "calendar-left-wing";
  }
  return "unknown";
}

const char* MmVerdictName(MmVerdict verdict) noexcept {
  switch (verdict) {
    case MmVerdict::kNoArbitrage:
      return "no-arbitrage";
    case MmVerdict::kInvalidParameters:
      return "invalid-parameters";
    case MmVerdict::kNonPositiveVariance:
      return "non-positive-variance";
    case MmVerdict::kType1:
      return "type-1";
    case MmVerdict::kType2:
      return "type-2";
    case MmVerdict::kType3:
      return "type-3";
    case MmVerdict::kType4:
      return "type-4";
    case MmVerdict::kUndecided:
      return "undecided";
  }
  return "unknown";
}

std::vector<double> MakeGrid(const GridSpec& spec, const SviParams* slices, std::size_t count) {
  std::vector<double> grid;
  const auto add_part = [&grid](double centre, double scale, double half_width, double step) {
    if (!(step > 0.0) || !(half_width >= 0.0) || !std::isfinite(centre) || !std::isfinite(scale)) {
      return;
    }
    const double steps = std::floor((half_width / step) + 1e-9);
    if (!(steps <= kMaxGridPart)) return;
    const auto n = static_cast<long long>(steps);
    for (long long i = -n; i <= n; ++i) {
      grid.push_back(centre + (scale * (static_cast<double>(i) * step)));
    }
  };
  add_part(0.0, 1.0, spec.k_half_width, spec.k_step);
  for (std::size_t i = 0; i < count && slices != nullptr; ++i) {
    add_part(slices[i].m, slices[i].sigma, spec.y_half_width, spec.y_step);
  }
  std::erase_if(grid, [](double k) { return !std::isfinite(k); });
  std::ranges::sort(grid);
  const auto [first, last] = std::ranges::unique(grid);
  grid.erase(first, last);
  return grid;
}

double DensityTolerance(const SviParams& p, double k) {
  return DensityToleranceAt(p, Evaluate(p, k), k);
}

std::vector<Violation> CheckParameters(const SviParams& p) {
  std::vector<Violation> out;
  const std::array<double, 5> values{p.a, p.b, p.rho, p.m, p.sigma};
  for (const double value : values) {
    if (!std::isfinite(value)) {
      out.push_back(Point(ViolationKind::kInvalidParameters, kNaN, value, 0.0));
      return out;
    }
  }
  const auto invalid = [&out](double value) {
    out.push_back(Point(ViolationKind::kInvalidParameters, kNaN, value, 0.0));
  };
  if (std::fabs(p.a) > kMaxAbsParameter) {
    invalid(p.a);
  } else if (p.b < 0.0 || p.b > kMaxAbsParameter) {
    invalid(p.b);
  } else if (std::fabs(p.rho) > 1.0) {
    invalid(p.rho);
  } else if (std::fabs(p.m) > kMaxAbsParameter) {
    invalid(p.m);
  } else if (p.sigma < kMinSigma || p.sigma > kMaxAbsParameter) {
    invalid(p.sigma);
  }
  if (!out.empty()) return out;

  if (std::fabs(p.rho) < 1.0) {
    const double root = std::sqrt((1.0 - p.rho) * (1.0 + p.rho));
    const double w_min = p.a + (p.b * p.sigma * root);
    if (!(w_min > 0.0)) {
      out.push_back(
          Point(ViolationKind::kNonPositiveVariance, p.m - (p.rho * p.sigma / root), w_min, 0.0));
    }
  } else if (p.a < 0.0 || (p.a == 0.0 && p.b == 0.0)) {
    // With |rho| = 1, w decreases (rho = -1) or increases (rho = 1) to a at the far wing.
    out.push_back(Point(ViolationKind::kNonPositiveVariance, p.rho < 0.0 ? kInf : -kInf, p.a, 0.0));
  }
  return out;
}

std::vector<Violation> CheckDensity(const SviParams& p, const std::vector<double>& grid) {
  return ScanDensity(p, grid).violations;
}

std::vector<Violation> CheckDPlusLimit(const SviParams& p) {
  const double slope = p.b * (1.0 + p.rho);
  if (slope < 2.0) return {};
  return {Point(ViolationKind::kDPlusLimit, kInf, slope, 2.0)};
}

std::vector<Violation> CheckWings(const SviParams& p, double bound) {
  std::vector<Violation> out;
  const double right = p.b * (1.0 + p.rho);
  const double left = p.b * (1.0 - p.rho);
  if (!(right < bound)) out.push_back(Point(ViolationKind::kRightWing, kInf, right, bound));
  if (!(left < bound)) out.push_back(Point(ViolationKind::kLeftWing, -kInf, left, bound));
  return out;
}

MartiniMingone ClassifyMartiniMingone(const SviParams& p) { return Classify(p).result; }

ButterflyReport CheckButterfly(const SviParams& p, const GridSpec& spec, double wing_bound) {
  ButterflyReport report;
  report.violations = CheckParameters(p);
  report.min_g = kNaN;
  report.k_min_g = kNaN;
  const Classified classified = Classify(p);
  report.classifier = classified.result;
  if (!report.violations.empty()) return report;

  const std::vector<double> grid = MakeGrid(spec, &p, 1);
  report.grid_points = grid.size();
  DensityScan scan = ScanDensity(p, grid);
  report.min_g = scan.min_g;
  report.k_min_g = scan.k_min_g;
  report.violations = std::move(scan.violations);
  for (const Violation& v : CheckDPlusLimit(p)) report.violations.push_back(v);
  for (const Violation& v : CheckWings(p, wing_bound)) report.violations.push_back(v);
  const MmVerdict verdict = classified.result.verdict;
  if (verdict != MmVerdict::kNoArbitrage) {
    report.violations.push_back(Point(ViolationKind::kMartiniMingone, classified.k_location,
                                      VerdictNumber(verdict), classified.threshold));
  }
  return report;
}

std::vector<Violation> CheckCalendarWings(const SviParams& earlier, const SviParams& later) {
  std::vector<Violation> out;
  // w ~ beta |k| + c + q / |k| as k -> side * infinity, with beta = b (1 + side rho), c = a -
  // side beta m, q = b sigma^2 / 2.
  for (const double side : {1.0, -1.0}) {
    const auto beta = [side](const SviParams& p) { return p.b * (1.0 + (side * p.rho)); };
    const auto c = [side, &beta](const SviParams& p) { return p.a - (side * beta(p) * p.m); };
    const auto c_scale = [&beta](const SviParams& p) {
      return std::fabs(p.a) + std::fabs(beta(p) * p.m);
    };
    const auto q = [](const SviParams& p) { return 0.5 * p.b * p.sigma * p.sigma; };
    const ViolationKind kind =
        side > 0.0 ? ViolationKind::kCalendarRightWing : ViolationKind::kCalendarLeftWing;
    const double at = side * kInf;
    const double d_slope = beta(later) - beta(earlier);
    const double t_slope = 4.0 * kEps * (beta(later) + beta(earlier));
    if (d_slope < -t_slope) {
      out.push_back(Point(kind, at, d_slope, 1.0));
      continue;
    }
    if (d_slope > t_slope) continue;
    const double d_intercept = c(later) - c(earlier);
    const double t_intercept = 8.0 * kEps * (c_scale(later) + c_scale(earlier));
    if (d_intercept < -t_intercept) {
      out.push_back(Point(kind, at, d_intercept, 0.0));
      continue;
    }
    if (d_intercept > t_intercept) continue;
    const double d_q = q(later) - q(earlier);
    const double t_q = 4.0 * kEps * (q(later) + q(earlier));
    if (d_q < -t_q) out.push_back(Point(kind, at, d_q, -1.0));
  }
  return out;
}

CalendarReport CheckCalendar(const SviParams& earlier, const SviParams& later,
                             const GridSpec& spec) {
  CalendarReport report;
  report.min_gap = kNaN;
  report.k_min_gap = kNaN;
  for (const SviParams* p : {&earlier, &later}) {
    for (const Violation& v : CheckParameters(*p)) report.violations.push_back(v);
  }
  if (!report.violations.empty()) return report;

  const std::array<SviParams, 2> slices{earlier, later};
  const std::vector<double> grid = MakeGrid(spec, slices.data(), slices.size());
  report.grid_points = grid.size();
  RunCollector runs(ViolationKind::kCalendarCrossing, &report.violations);
  double min_gap = kInf;
  for (const double k : grid) {
    const double gap = TotalVariance(later, k) - TotalVariance(earlier, k);
    if (gap < min_gap) {
      min_gap = gap;
      report.k_min_gap = k;
    }
    const double tolerance = 16.0 * kEps * (WMagnitude(earlier, k) + WMagnitude(later, k));
    if (gap < -tolerance) {
      runs.Fail(k, gap, -tolerance);
    } else {
      runs.Pass();
    }
  }
  runs.Pass();
  if (!grid.empty()) report.min_gap = min_gap;
  for (const Violation& v : CheckCalendarWings(earlier, later)) report.violations.push_back(v);
  return report;
}

}  // namespace aadp::svi
