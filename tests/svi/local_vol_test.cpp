// Dupire local variance and the floor rule (svi/local_vol.hpp) against docs/NOTES.md, "Stage 7a",
// whose pre-registration states every tolerance here before this test existed.
//
// The Dupire check has two parts.
// 1. Finite differences of dT w and g, on synthetic surfaces A and B (tests/svi/
//    synthetic_surface.hpp), k from -0.3 to 0.3 in steps of 0.01, t at 0.1, 0.5 and 0.9 of each
//    segment, and t = 0 for g. dT w by a central difference in t, h_t = 2^-10 times the segment,
//    within 2^-30 S_N, S_N = (|w_{j-1}| + |w_j|) / (T_j - T_{j-1}), or |w_0| / T_0 before the
//    first slice: w is linear in t inside a segment, so the difference has no truncation. g from
//    w and central differences w', w'' in k, h_k = 2^-10 min sigma of the slices involved,
//    through raw_svi.hpp's own form of GJ eq. (2.1), within 1e-6 S_g, S_g the sum of the four
//    terms' magnitudes, plus the differences' own rounding R (docs/NOTES.md, "Stage 7a", decisions
//    after the pre-registration): E = 2^-51 S_w bounds each evaluation of w, S_w the magnitudes
//    of its terms, |a| + b (|rho x| + r) per slice, weighted as w is; then w' errs by E / h_k
//    and w'' by 4 E / h_k^2, so R = E (|dg/dw| + |dg/dw'| / h_k + 2 / h_k^2). The bound as
//    registered left R out, and failed on B's left wing, where rho x + r cancels.
// 2. Dupire in call prices, on A with F = 100 and D = 1, where sigma_loc^2 = 2 dT C / (K^2 dKK C):
//    C by Black76Price (Let's Be Rational), the out-of-the-money option, t at the three segment
//    midpoints, k from -0.3 to 0.3 in steps of 0.01 where the point is not floored or capped,
//    g >= 0.05 and |k| / sqrt(w) <= 2.5, h_K = 2^-9 K sqrt(w), h_T = 2^-10 times the segment,
//    within 1e-4 relative. Part 1 shares nothing with the local variance's code but the surface,
//    and part 2 sees a wrong denominator, such as VW4's displayed sign, that part 1 would too.
#include "svi/local_vol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

#include "ad/real.hpp"
#include "ad/tape.hpp"
#include "svi/arbitrage.hpp"
#include "svi/implied_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"
#include "svi/synthetic_surface.hpp"

namespace aadp::svi {
namespace {

using testing::Surface;

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr LocalVolRule kRule{};

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

LocalVariance<double> At(const SviSurface<double>& s, double t, double k) {
  return LocalVarianceAt(s.At(s.Locate(t), k), k, kRule);
}

TEST(FloorRule, ClassifiesInTheRegisteredOrder) {
  const double omega = kAdmissibleThreshold;
  EXPECT_EQ(Classify(kNan, 1.0, 1.0, kRule), LocalVarianceClass::kNonFinite);
  EXPECT_EQ(Classify(1.0, std::numeric_limits<double>::infinity(), 1.0, kRule),
            LocalVarianceClass::kNonFinite);
  EXPECT_EQ(Classify(1.0, 1.0, kNan, kRule), LocalVarianceClass::kNonFinite);
  EXPECT_EQ(Classify(omega, 1.0, 1.0, kRule), LocalVarianceClass::kNonPositiveVariance);
  EXPECT_EQ(Classify(-1.0, -1.0, -1.0, kRule), LocalVarianceClass::kNonPositiveVariance);
  EXPECT_EQ(Classify(1.0, omega, 1.0, kRule), LocalVarianceClass::kCalendar);
  EXPECT_EQ(Classify(1.0, -1.0, -1.0, kRule), LocalVarianceClass::kCalendar);
  EXPECT_EQ(Classify(1.0, 1.0, omega, kRule), LocalVarianceClass::kButterfly);
  EXPECT_EQ(Classify(1.0, 1.0, -2.0, kRule), LocalVarianceClass::kButterfly);
  EXPECT_EQ(Classify(1.0, kRule.floor, 1.0, kRule), LocalVarianceClass::kBelowFloor);
  EXPECT_EQ(Classify(1.0, kRule.cap, 1.0, kRule), LocalVarianceClass::kCapped);
  EXPECT_EQ(Classify(1.0, 1e300, 2.0 * omega, kRule), LocalVarianceClass::kCapped);
  EXPECT_EQ(Classify(1.0, 0.04, 1.0, kRule), LocalVarianceClass::kOk);
  EXPECT_TRUE(IsFloored(LocalVarianceClass::kBelowFloor));
  EXPECT_FALSE(IsFloored(LocalVarianceClass::kCapped));
  EXPECT_FALSE(IsFloored(LocalVarianceClass::kOk));
}

// Two flat slices (b = 0) at 0.5 and 1: g = 1 exactly, and dT w = (a_1 - a_0) / 0.5.
SviSurface<double> TwoFlat(double a0, double a1) {
  const std::array<SliceTerms, 2> terms = {{{.expiry = 0.5, .forward = 100.0, .discount = 1.0},
                                            {.expiry = 1.0, .forward = 100.0, .discount = 1.0}}};
  const std::array<RawSvi<double>, 2> params = {
      {{.a = a0, .b = 0.0, .rho = 0.0, .m = 0.0, .sigma = 0.1},
       {.a = a1, .b = 0.0, .rho = 0.0, .m = 0.0, .sigma = 0.1}}};
  EXPECT_EQ(ValidateSurface(terms, params), SurfaceError::kNone);
  return {terms, params};
}

TEST(FloorRule, EachClassOccursWhereTheSurfaceSaysAndTakesTheFloorOrTheCap) {
  // Capped: dT w / g = 10 > 4.
  const LocalVariance<double> capped = At(TwoFlat(0.02, 5.02), 0.75, 0.05);
  EXPECT_EQ(capped.cls, LocalVarianceClass::kCapped);
  EXPECT_EQ(capped.variance, kRule.cap);
  // Below the floor: 2e-5 < 1e-4.
  const LocalVariance<double> low = At(TwoFlat(0.02, 0.02001), 0.75, 0.05);
  EXPECT_EQ(low.cls, LocalVarianceClass::kBelowFloor);
  EXPECT_EQ(low.variance, kRule.floor);
  // Calendar: the later slice below the earlier.
  const LocalVariance<double> calendar = At(TwoFlat(0.04, 0.03), 0.75, 0.05);
  EXPECT_EQ(calendar.cls, LocalVarianceClass::kCalendar);
  EXPECT_EQ(calendar.variance, kRule.floor);
  // Non-positive variance: a = b = 0 before the first slice.
  const LocalVariance<double> zero = At(TwoFlat(0.0, 0.04), 0.25, 0.05);
  EXPECT_EQ(zero.cls, LocalVarianceClass::kNonPositiveVariance);
  EXPECT_EQ(zero.variance, kRule.floor);
  // Admissible: exactly dT w / g.
  const LocalVariance<double> ok = At(TwoFlat(0.02, 0.06), 0.75, 0.05);
  EXPECT_EQ(ok.cls, LocalVarianceClass::kOk);
  EXPECT_EQ(Bits(ok.variance), Bits(ok.dwdt / ok.g));

  // Butterfly: Vogt's slice (GJ Example 3.1) alone at T = 1, just before it, where its own g is
  // clearly negative.
  const std::array<SliceTerms, 1> vogt_terms = {
      {{.expiry = 1.0, .forward = 100.0, .discount = 1.0}}};
  const std::array<RawSvi<double>, 1> vogt = {
      {{.a = -0.0410, .b = 0.1331, .rho = 0.3060, .m = 0.3586, .sigma = 0.4153}}};
  const SviSurface<double> v(vogt_terms, vogt);
  std::size_t butterflies = 0;
  for (int i = -150; i <= 150; ++i) {
    const double k = 0.01 * i;
    if (Durrleman(vogt[0], k) < -0.01) {
      const LocalVariance<double> lv = At(v, 0.999, k);
      EXPECT_EQ(lv.cls, LocalVarianceClass::kButterfly) << "k " << k;
      EXPECT_EQ(lv.variance, kRule.floor);
      butterflies += 1;
    }
  }
  EXPECT_GT(butterflies, 10U);

  // Calendar on B, between its first two slices: exactly where the later slice is lower.
  const Surface b = testing::SurfaceB();
  const SviSurface<double> sb(b.terms, b.params);
  const double t = 0.5 * (b.terms[0].expiry + b.terms[1].expiry);
  std::size_t crossings = 0;
  for (int i = -30; i <= 30; ++i) {
    const double k = 0.01 * i;
    const bool lower = TotalVariance(b.params[1], k) < TotalVariance(b.params[0], k);
    const LocalVariance<double> lv = At(sb, t, k);
    if (lower) {
      EXPECT_EQ(lv.cls, LocalVarianceClass::kCalendar) << "k " << k;
      crossings += 1;
    } else {
      EXPECT_NE(lv.cls, LocalVarianceClass::kCalendar) << "k " << k;
    }
  }
  EXPECT_GT(crossings, 5U);
}

TEST(LocalVariance, AFlatSurfaceGivesItsConstantVariance) {
  const Surface flat = testing::FlatSurface();
  const SviSurface<double> s(flat.terms, flat.params);
  const double want = testing::kFlatVol * testing::kFlatVol;
  for (const double t : {0.0, 0.005, 0.013, 0.02, 0.05, 0.09}) {
    for (int i = -10; i <= 10; ++i) {
      const double k = 0.05 * i;
      const LocalVariance<double> lv = At(s, t, k);
      ASSERT_EQ(lv.cls, LocalVarianceClass::kOk) << t << ' ' << k;
      ASSERT_EQ(lv.g, 1.0);
      ASSERT_LE(std::fabs(lv.variance / want - 1.0), 8.0 * 0x1p-52) << t << ' ' << k;
    }
  }
}

double SegmentStart(const Surface& s, std::size_t segment) {
  return segment == 0 ? 0.0 : s.terms[segment - 1].expiry;
}

double MinSigma(const Surface& s, std::size_t segment) {
  const double hi = s.params[segment].sigma;
  return segment == 0 ? hi : std::min(hi, s.params[segment - 1].sigma);
}

// Magnitudes of w's terms for one slice: what its evaluation's rounding scales with.
double TermScale(const RawSvi<double>& p, double k) {
  const double x = k - p.m;
  return std::fabs(p.a) + (p.b * (std::fabs(p.rho * x) + std::sqrt((x * x) + (p.sigma * p.sigma))));
}

// R above, for g from (w, w', w'') with w' and w'' from differences at h of values each
// within E. second_order = false for t = 0, where g = u^2 uses w' only.
double DifferenceRounding(double w, double w1, double k, double e, double h, bool second_order) {
  const double q = w1 / w;
  const double u = 1.0 - (0.5 * k * q);
  const double dg_dq = second_order ? (-k * u) - (0.25 * w1) : -k * u;
  const double dg_dw = std::fabs(dg_dq * q / w);
  const double dg_dw1 = std::fabs((dg_dq / w) - (second_order ? (0.25 * q) + (0.125 * w1) : 0.0));
  return e * (dg_dw + (dg_dw1 / h) + (second_order ? 2.0 / (h * h) : 0.0));
}

struct Worst {
  double dwdt = 0.0;
  double g = 0.0;
  double g_registered = 0.0;  // against 1e-6 S_g alone, as registered
  std::size_t beyond_registered = 0;
  double beyond_k_min = std::numeric_limits<double>::infinity();
  double beyond_k_max = -std::numeric_limits<double>::infinity();
  std::size_t points = 0;
};

// Part 1 at one (t, k).
void CheckFiniteDifferences(const Surface& s, const SviSurface<double>& surface,
                            std::size_t segment, double t, double k, Worst& worst) {
  const double lo = SegmentStart(s, segment);
  const double hi = s.terms[segment].expiry;
  const TimeSlot slot = surface.Locate(t);
  ASSERT_EQ(slot.segment, segment);
  const SurfacePoint<double> p = surface.At(slot, k);
  if (!(p.q_den > kAdmissibleThreshold)) return;
  const LocalVariance<double> lv = LocalVarianceAt(p, k, kRule);
  const double h_k = 0x1p-10 * MinSigma(s, segment);
  double w = 0.0;
  double w1 = 0.0;
  double w2 = 0.0;
  if (t == 0.0) {
    // g's limit at t = 0, from slice 0 alone: (1 - k w_0' / (2 w_0))^2.
    const RawSvi<double>& p0 = s.params[0];
    const double up = TotalVariance(p0, k + h_k);
    const double down = TotalVariance(p0, k - h_k);
    const double w0 = TotalVariance(p0, k);
    const double u = 1.0 - (k * ((up - down) / (2.0 * h_k)) / (2.0 * w0));
    const double g_fd = u * u;
    const double scale = u * u + 1.0;
    const double w0_1 = EvaluateSlice(MakeSliceSetup(p0), k).w1;
    const double tol =
        (1e-6 * scale) + DifferenceRounding(w0, w0_1, k, 0x1p-51 * TermScale(p0, k), h_k, false);
    ASSERT_LE(std::fabs(g_fd - lv.g), tol) << "t 0, k " << k;
    worst.g = std::max(worst.g, std::fabs(g_fd - lv.g) / tol);
    worst.points += 1;
    return;
  }
  const double h_t = 0x1p-10 * (hi - lo);
  const double w_up = surface.At(surface.Locate(t + h_t), k).w;
  const double w_down = surface.At(surface.Locate(t - h_t), k).w;
  const double dwdt_fd = (w_up - w_down) / (2.0 * h_t);
  const double s_n = segment == 0 ? std::fabs(TotalVariance(s.params[0], k)) / hi
                                  : (std::fabs(TotalVariance(s.params[segment - 1], k)) +
                                     std::fabs(TotalVariance(s.params[segment], k))) /
                                        (hi - lo);
  const double dwdt_tol = 0x1p-30 * s_n;
  ASSERT_LE(std::fabs(dwdt_fd - p.dwdt), dwdt_tol)
      << "segment " << segment << ", t " << t << ", k " << k;
  worst.dwdt = std::max(worst.dwdt, std::fabs(dwdt_fd - p.dwdt) / dwdt_tol);

  w = p.w;
  const double up = surface.At(slot, k + h_k).w;
  const double down = surface.At(slot, k - h_k).w;
  w1 = (up - down) / (2.0 * h_k);
  w2 = (up - (2.0 * w) + down) / (h_k * h_k);
  const double g_fd = Durrleman(SviPoint<double>{.w = w, .w1 = w1, .w2 = w2}, k);
  const double u = 1.0 - (k * p.w1 / (2.0 * p.w));
  const double s_g =
      (u * u) + std::fabs(p.w1 * p.w1 / (4.0 * p.w)) + (p.w1 * p.w1 / 16.0) + std::fabs(p.w2 / 2.0);
  const double alpha = slot.weight;
  const double s_w = segment == 0 ? alpha * TermScale(s.params[0], k)
                                  : ((1.0 - alpha) * TermScale(s.params[segment - 1], k)) +
                                        (alpha * TermScale(s.params[segment], k));
  const double tol = (1e-6 * s_g) + DifferenceRounding(p.w, p.w1, k, 0x1p-51 * s_w, h_k, true);
  ASSERT_LE(std::fabs(g_fd - lv.g), tol) << "segment " << segment << ", t " << t << ", k " << k;
  worst.g = std::max(worst.g, std::fabs(g_fd - lv.g) / tol);
  worst.g_registered = std::max(worst.g_registered, std::fabs(g_fd - lv.g) / (1e-6 * s_g));
  if (std::fabs(g_fd - lv.g) > 1e-6 * s_g) {
    worst.beyond_registered += 1;
    worst.beyond_k_min = std::min(worst.beyond_k_min, k);
    worst.beyond_k_max = std::max(worst.beyond_k_max, k);
  }
  worst.points += 1;
}

TEST(Dupire, FiniteDifferencesOfTheTimeDerivativeAndOfGAgree) {
  for (const auto& [name, s] :
       {std::pair{"A", testing::SurfaceA()}, std::pair{"B", testing::SurfaceB()}}) {
    const SviSurface<double> surface(s.terms, s.params);
    Worst worst;
    for (int i = -30; i <= 30; ++i) {
      const double k = 0.01 * i;
      CheckFiniteDifferences(s, surface, 0, 0.0, k, worst);
      for (std::size_t segment = 0; segment < 3; ++segment) {
        const double lo = SegmentStart(s, segment);
        const double hi = s.terms[segment].expiry;
        for (const double fraction : {0.1, 0.5, 0.9}) {
          CheckFiniteDifferences(s, surface, segment, lo + (fraction * (hi - lo)), k, worst);
        }
      }
    }
    EXPECT_GT(worst.points, 500U) << name;
    std::printf(
        "measured, surface %s: %zu points, worst |dT w fd - dT w| / bound %.3g, "
        "worst |g fd - g| / bound %.3g (/ 1e-6 S_g alone, as registered, %.3g, beyond it at %zu "
        "points, k from %.2f to %.2f)\n",
        name, worst.points, worst.dwdt, worst.g, worst.g_registered, worst.beyond_registered,
        worst.beyond_k_min, worst.beyond_k_max);
  }
}

// The undiscounted Black price at (K, T) on a surface with F constant, D = 1. The type is the
// out-of-the-money one at the centre of the difference, and the same at every point of it: a put
// at K - h and a call at K would add (F - K + h) / h^2 to the second difference.
double Price(const SviSurface<double>& s, double forward, double strike, double t,
             OptionType type) {
  const double k = std::log(strike / forward);
  const double w = s.At(s.Locate(t), k).w;
  return Black76Price(forward, 1.0, strike, t, std::sqrt(w / t), type);
}

TEST(Dupire, LocalVarianceIsDupiresInCallPrices) {
  const Surface s = testing::SurfaceAFlatForward();
  // A is free of static arbitrage on the region checked, by stage 6a's checks.
  for (const RawSvi<double>& p : s.params) {
    ASSERT_TRUE(CheckButterfly(p).violations.empty());
  }
  ASSERT_TRUE(CheckCalendar(s.params[0], s.params[1]).violations.empty());
  ASSERT_TRUE(CheckCalendar(s.params[1], s.params[2]).violations.empty());
  const SviSurface<double> surface(s.terms, s.params);
  const double forward = 100.0;
  double worst = 0.0;
  std::size_t points = 0;
  for (std::size_t segment = 0; segment < 3; ++segment) {
    const double lo = SegmentStart(s, segment);
    const double hi = s.terms[segment].expiry;
    const double t = 0.5 * (lo + hi);
    const double h_t = 0x1p-10 * (hi - lo);
    std::size_t here = 0;
    for (int i = -30; i <= 30; ++i) {
      const double k = 0.01 * i;
      const SurfacePoint<double> p = surface.At(surface.Locate(t), k);
      const LocalVariance<double> lv = LocalVarianceAt(p, k, kRule);
      if (lv.cls != LocalVarianceClass::kOk || lv.g < 0.05 || std::fabs(k) / std::sqrt(p.w) > 2.5) {
        continue;
      }
      const double strike = forward * std::exp(k);
      const double h_k = 0x1p-9 * strike * std::sqrt(p.w);
      const OptionType type = k < 0.0 ? OptionType::kPut : OptionType::kCall;
      const double c = Price(surface, forward, strike, t, type);
      const double dcdt = (Price(surface, forward, strike, t + h_t, type) -
                           Price(surface, forward, strike, t - h_t, type)) /
                          (2.0 * h_t);
      const double dcdkk = (Price(surface, forward, strike + h_k, t, type) - (2.0 * c) +
                            Price(surface, forward, strike - h_k, t, type)) /
                           (h_k * h_k);
      const double dupire = 2.0 * dcdt / (strike * strike * dcdkk);
      const double error = std::fabs(dupire / lv.variance - 1.0);
      ASSERT_LE(error, 1e-4) << "segment " << segment << ", k " << k << ": Dupire " << dupire
                             << ", dT w / g " << lv.variance;
      worst = std::max(worst, error);
      here += 1;
    }
    EXPECT_GE(here, 5U) << "segment " << segment;
    points += here;
  }
  std::printf("measured: %zu points, worst |Dupire in prices / (dT w / g) - 1| %.3g (bound 1e-4)\n",
              points, worst);
}

// The tape gives the double's bits, and a finite gradient everywhere: at a floored point the
// branch Select did not take must not leave an infinite partial on the tape.
TEST(LocalVariance, TheTapeGivesTheDoubleBitsAndAFiniteGradientEvenWhereFloored) {
  using Seen = std::array<std::size_t, kLocalVarianceClasses>;
  const auto check = [](const std::vector<SliceTerms>& terms,
                        const std::vector<RawSvi<double>>& params, const std::vector<double>& times,
                        Seen& seen) {
    const SviSurface<double> plain(terms, params);
    ad::Tape tape;
    const ad::TapeScope scope(tape);
    std::vector<RawSvi<ad::Real>> active;
    std::vector<ad::Real> inputs;
    for (const RawSvi<double>& p : params) {
      for (const double x : {p.a, p.b, p.rho, p.m, p.sigma}) inputs.push_back(tape.Register(x));
      const std::size_t i = inputs.size() - 5;
      active.push_back({.a = inputs[i],
                        .b = inputs[i + 1],
                        .rho = inputs[i + 2],
                        .m = inputs[i + 3],
                        .sigma = inputs[i + 4]});
    }
    const SviSurface<ad::Real> surface(terms, active);
    for (const double t : times) {
      for (int i = -20; i <= 20; ++i) {
        const double k = 0.02 * i;
        const ad::Position start = tape.GetPosition();
        const LocalVariance<double> want = At(plain, t, k);
        const TimeSlot slot = plain.Locate(t);
        const SurfacePoint<ad::Real> p = surface.At(slot, ad::Real(k));
        const LocalVariance<ad::Real> got = LocalVarianceAt(p, ad::Real(k), kRule);
        ASSERT_EQ(got.cls, want.cls);
        ASSERT_EQ(Bits(got.variance.value()), Bits(want.variance));
        seen[static_cast<std::size_t>(got.cls)] += 1;
        // The output w + v is active whether or not v is floored.
        const ad::Real out = p.w + got.variance;
        tape.AddAdjoint(out, 1.0);
        tape.SweepBack(tape.GetPosition(), ad::Position{});
        for (const ad::Real& x : inputs) {
          ASSERT_TRUE(std::isfinite(tape.Adjoint(x)))
              << "t " << t << ", k " << k << ", class " << LocalVarianceClassName(got.cls);
        }
        tape.ClearAdjoints(tape.GetPosition(), ad::Position{});
        tape.Rewind(start);
      }
    }
  };
  const Surface b = testing::SurfaceB();
  Seen on_b{};
  check({b.terms.begin(), b.terms.end()}, {b.params.begin(), b.params.end()},
        {0.0, 0.006, 0.02, 0.05, 0.09}, on_b);
  EXPECT_GT(on_b[static_cast<std::size_t>(LocalVarianceClass::kOk)], 0U);
  EXPECT_GT(on_b[static_cast<std::size_t>(LocalVarianceClass::kCalendar)], 0U);
  EXPECT_GT(on_b[static_cast<std::size_t>(LocalVarianceClass::kButterfly)], 0U);
  // w = 0 exactly before the first slice: q = w' / w would be 0 / 0, with infinite partials.
  Seen on_zero{};
  check({{.expiry = 0.5, .forward = 100.0, .discount = 1.0}},
        {{.a = 0.0, .b = 0.0, .rho = 0.0, .m = 0.0, .sigma = 0.1}}, {0.0, 0.25}, on_zero);
  EXPECT_GT(on_zero[static_cast<std::size_t>(LocalVarianceClass::kNonPositiveVariance)], 0U);
}

}  // namespace
}  // namespace aadp::svi
