// Static-arbitrage checks for raw SVI slices (BENCHMARKS.md SVI-3, SVI-4, SVI-5, SVI-9).
//
// None of them repairs anything. Each returns every violation it finds, with its location in
// log-moneyness k = ln(K / F) and its value, and leaves the parameters alone.
//
// Butterfly, one slice (CheckButterfly):
// - parameters: finite, b >= 0, |rho| <= 1, sigma > 0, and inside the evaluated domain below;
// - positive total variance: min w = a + b sigma sqrt(1 - rho^2) > 0 at k* = m - rho sigma /
//   sqrt(1 - rho^2) when |rho| < 1, a >= 0 when |rho| = 1 (Martini and Mingone, arXiv 2005.03340,
//   section 2.2: a smile that touches zero has a butterfly arbitrage);
// - g(k) >= -eps_g(k) at every point of a grid (GridSpec), g from Gatheral and Jacquier, arXiv
//   1204.0646, eq. (2.1), with GJ Lemma 2.2. Consecutive failing grid points are one violation,
//   reported with its first and last k, the worst k and the worst g;
// - the d+ limit of GJ Lemma 2.2: d+(k) -> -infinity as k -> +infinity, which for SVI holds if
//   and only if b (1 + rho) < 2 (MM Lemma 2.1: the limit is 0 at 2 and +infinity above);
// - Lee's wings at the moment-formula slope 2, not QuantLib's 4: b (1 + rho) < bound and
//   b (1 - rho) < bound, bound 2 by default (MM section 2.3 and Proposition 2.2; BENCHMARKS.md
//   SVI-4). The bound is a parameter only so that a test can show what 4 lets through;
// - the Martini and Mingone classifier (ClassifyMartiniMingone), their necessary and sufficient
//   condition for no butterfly arbitrage under MM's axioms (Theorems 5.10, 6.2 and 6.3, with the
//   failure waterfall of sections 5.5 and 6.3). MM's axioms do not require call prices to vanish
//   at infinity, so at b (1 + rho) = 2 exactly the classifier can pass a slice that the d+ check,
//   on GJ's definition, rejects. Both are reported.
//
// The grid covers the region where data and pricing live, many times over; the asymptotic wings
// are covered analytically by the d+ and wing checks (g(k) tends to 1/4 - beta^2 / 16 with beta
// the wing slope, so g >= 0 at +-infinity exactly when the slopes are at most 2), and the
// classifier decides the whole real line. A negative region narrower than the grid spacing, or
// lying past the grid between it and the asymptote, can escape the grid but not the classifier.
//
// Calendar, two slices (CheckCalendar): no calendar spread arbitrage between expiries T1 < T2
// means w(k, T2) >= w(k, T1) for every k (GJ Lemma 2.1 and Definition 2.1, proportional
// dividends). No necessary and sufficient condition on two raw SVI parameter sets is known:
// Martini and Mingone list "the characterization of no Calendar Spread arbitrage for two SVI
// slices corresponding to different maturities" as open (conclusion; BENCHMARKS.md SVI-5). GJ
// Lemma 3.3's quartic is sufficient only. So this check is numerical: a dense crossing test on
// a grid, plus an analytic comparison of the asymptotes, w ~ beta |k| + c + q / |k| as k ->
// +-infinity, which covers |k| -> infinity. It can miss a crossing narrower than the grid spacing
// outside the grid's dense parts, and it cannot decide two slices whose asymptotes agree in
// slope, intercept and 1/|k| coefficient (they differ by O(1/k^2) there); neither case is
// reported as a violation.
#ifndef AADP_SVI_ARBITRAGE_HPP_
#define AADP_SVI_ARBITRAGE_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "svi/raw_svi.hpp"

namespace aadp::svi {

using SviParams = RawSvi<double>;

// Inside this domain every grid evaluation of w, w', w'' and g is finite and the grid resolves
// sigma; outside it the parameters are reported invalid rather than evaluated. Total variance of
// 1e4 is an implied volatility of 100 at T = 1.
inline constexpr double kMaxAbsParameter = 1e4;
inline constexpr double kMinSigma = 1e-8;

enum class ViolationKind : std::uint8_t {
  kInvalidParameters,    // value: the first offending parameter's value
  kNonPositiveVariance,  // value: min w, at k* (or at -+infinity when |rho| = 1)
  kDensity,              // value: g at k_worst; bound: -eps_g there
  kNonFiniteDensity,     // g evaluated to NaN or infinity on the grid; never expected
  kDPlusLimit,           // value: b (1 + rho), at k = +infinity
  kRightWing,            // value: b (1 + rho), at k = +infinity; bound: the slope bound
  kLeftWing,             // value: b (1 - rho), at k = -infinity; bound: the slope bound
  kMartiniMingone,       // value: the failure type 1 to 4, or 0 when undecided
  kCalendarCrossing,     // value: w_later - w_earlier at k_worst; bound: -eps_w there
  kCalendarRightWing,    // value: the later slice's asymptote minus the earlier's, at the order
  kCalendarLeftWing,     //   that decides (bound names it: 1 slope, 0 intercept, -1 1/|k|)
};

[[nodiscard]] const char* KindName(ViolationKind kind) noexcept;

struct Violation {
  ViolationKind kind = ViolationKind::kInvalidParameters;
  double k_begin = 0.0;  // first and last grid point of the region; +-infinity for a wing
  double k_end = 0.0;
  double k_worst = 0.0;
  double value = 0.0;
  double bound = 0.0;
  std::size_t points = 0;  // grid points in the region; 0 for an analytic check
};

// The grid: k_step-spaced points on [-k_half_width, k_half_width], joined with, for each slice,
// sigma * y_step-spaced points on m + sigma [-y_half_width, y_half_width], sorted, duplicates
// dropped. The dense part around m resolves w'', whose width is sigma, with 100 points per sigma.
// The defaults: [-3, 3] at 0.001 is about e^-3 to e^3 times the forward, far beyond any quoted
// strike (the calibration set's strikes lie within |k| < 0.25), and y to +-40.
struct GridSpec {
  double k_half_width = 3.0;
  double k_step = 1e-3;
  double y_half_width = 40.0;
  double y_step = 1e-2;
};

[[nodiscard]] std::vector<double> MakeGrid(const GridSpec& spec, const SviParams* slices,
                                           std::size_t count);

// eps_g(k) = 32 eps c_w S, with eps = 2^-52, S = (1 + |k w'/(2w)|)^2 + (w'^2/4)(1/w + 1/4) +
// |w''|/2 the sum of the magnitudes of g's terms, and c_w = (|a| + b |rho (k - m)| + b r) / w the
// condition of w's sum. Each of g's terms is about ten operations from w, w' and w'', each with
// relative error a few eps, and w carries c_w eps; 32 bounds that with room. It is about 1e-14 on
// ordinary slices, ten orders below the violations it has to find (Vogt's g reaches -0.033).
[[nodiscard]] double DensityTolerance(const SviParams& p, double k);

enum class MmVerdict : std::uint8_t {
  kNoArbitrage,
  kInvalidParameters,
  kNonPositiveVariance,
  kType1,  // b (1 - rho) > 2 or b (1 + rho) > 2
  kType2,  // alpha <= F(b, rho): the Fukasawa interval for mu is empty
  kType3,  // mu outside the Fukasawa interval I_{alpha, b, rho}
  kType4,  // sigma < sigma*(alpha, b, rho, mu)
  kUndecided,
};

[[nodiscard]] const char* MmVerdictName(MmVerdict verdict) noexcept;

// MM's normalisation: SVI(k) = sigma N((k - m) / sigma), N(l) = alpha + b (rho l + sqrt(l^2 + 1)),
// alpha = a / sigma, mu = m / sigma (MM section 4).
struct MartiniMingone {
  MmVerdict verdict = MmVerdict::kUndecided;
  double alpha = 0.0;
  double mu = 0.0;
  // F(b, rho), MM section 5.2.2 (and 5.2.5 for b (1 +- rho) = 2), by bisection on alpha. NaN
  // where it is not computed: type 1, |rho| = 1, b = 0.
  double fukasawa_threshold = 0.0;
  // I_{alpha, b, rho} = ]sup L-, inf L+[ (MM Lemma 5.2, Theorem 5.10). For rho = -1 the upper
  // end is +infinity (MM Theorem 6.3). NaN where not reached.
  double mu_lower = 0.0;
  double mu_upper = 0.0;
  // The zeros l1 < 0 < l2 of G2 (MM Lemma 6.1; only l1 for rho = -1) and sigma* = sup of
  // -G2 / (2 G1) beyond them (MM section 6.2). NaN where not reached.
  double l1 = 0.0;
  double l2 = 0.0;
  double sigma_star = 0.0;
  // rho = +1 is decided as rho = -1 on k -> -k (mu -> -mu), which maps g(k) to g(-k) and so
  // keeps the verdict. The numbers above are then those of the reflected slice.
  bool reflected = false;
};

// sigma* is the maximum of -G2(1/h) / (2 G1(1/h)) over a grid of kMmSigmaGrid points in h on each
// side, refined by golden section around the largest: MM's own suggested computation (section
// 6.2.1), with MM's caveat that a single maximum per side is conjectured, not proved.
inline constexpr std::size_t kMmSigmaGrid = 4096;

[[nodiscard]] MartiniMingone ClassifyMartiniMingone(const SviParams& p);

// The pieces of CheckButterfly, for tests. Parameter and positivity violations come first; when
// there are any, the other checks are not run on those parameters.
[[nodiscard]] std::vector<Violation> CheckParameters(const SviParams& p);
[[nodiscard]] std::vector<Violation> CheckDensity(const SviParams& p,
                                                  const std::vector<double>& grid);
[[nodiscard]] std::vector<Violation> CheckDPlusLimit(const SviParams& p);
[[nodiscard]] std::vector<Violation> CheckWings(const SviParams& p, double bound = 2.0);

struct ButterflyReport {
  std::vector<Violation> violations;
  MartiniMingone classifier;
  std::size_t grid_points = 0;
  double min_g = 0.0;  // on the grid; NaN if the grid was not evaluated
  double k_min_g = 0.0;
};

[[nodiscard]] ButterflyReport CheckButterfly(const SviParams& p, const GridSpec& spec = {},
                                             double wing_bound = 2.0);

// Asymptotic comparison only, both wings, for tests.
[[nodiscard]] std::vector<Violation> CheckCalendarWings(const SviParams& earlier,
                                                        const SviParams& later);

struct CalendarReport {
  std::vector<Violation> violations;
  std::size_t grid_points = 0;
  double min_gap = 0.0;  // min of w_later - w_earlier on the grid
  double k_min_gap = 0.0;
};

// earlier is the slice of the shorter expiry. Both must pass CheckParameters; if either does not,
// the report holds those parameter violations and nothing else.
[[nodiscard]] CalendarReport CheckCalendar(const SviParams& earlier, const SviParams& later,
                                           const GridSpec& spec = {});

}  // namespace aadp::svi

#endif  // AADP_SVI_ARBITRAGE_HPP_
