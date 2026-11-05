// The static-arbitrage checks (src/svi/arbitrage.hpp) on known violators and on published
// arbitrage-free slices (BENCHMARKS.md SVI-3, SVI-4, SVI-5).
//
// Sources:
// - GJ: Gatheral and Jacquier, "Arbitrage-free SVI volatility surfaces", arXiv 1204.0646.
// - MM: Martini and Mingone, "No arbitrage SVI", arXiv 2005.03340.
// Published numbers are given to five decimals, so they are matched within 5e-6, half a unit in
// their last digit.
#include "svi/arbitrage.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "svi/raw_svi.hpp"

namespace aadp::svi {
namespace {

constexpr double kPublished = 5e-6;
constexpr double kInf = std::numeric_limits<double>::infinity();

// GJ eq. (3.8), t = 1.
constexpr SviParams kVogt{.a = -0.0410, .b = 0.1331, .rho = 0.3060, .m = 0.3586, .sigma = 0.4153};

// MM Table 1, the arbitrage-free rows, (a, b, rho, m, sigma).
constexpr std::array<SviParams, 6> kMmTable1{{
    {.a = 0.10, .b = 1.0, .rho = -0.306, .m = 0.10, .sigma = 0.30},
    {.a = -0.10, .b = 1.1, .rho = 0.200, .m = 0.00, .sigma = 0.60},
    {.a = 0.01, .b = 0.1, .rho = -0.600, .m = -0.05, .sigma = 0.10},
    {.a = 0.80, .b = 0.2, .rho = 0.800, .m = 1.00, .sigma = 0.90},
    {.a = 1.40, .b = 1.9, .rho = 0.000, .m = -0.10, .sigma = 0.50},
    {.a = 0.90, .b = 1.2, .rho = 0.500, .m = 0.20, .sigma = 0.85},
}};

// GJ Lemma 3.2: raw parameters of the SVI-JW slice (v, psi, p, c, v~) at expiry t.
SviParams FromJumpWings(double v, double psi, double p, double c, double v_tilde, double t) {
  const double w = v * t;
  const double root_w = std::sqrt(w);
  const double b = 0.5 * root_w * (c + p);
  const double rho = 1.0 - (p * root_w / b);
  const double beta = rho - (2.0 * psi * root_w / b);
  const double alpha = std::copysign(std::sqrt((1.0 / (beta * beta)) - 1.0), beta);
  const double root_rho = std::sqrt(1.0 - (rho * rho));
  const double m = (v - v_tilde) * t /
                   (b * (-rho + (std::copysign(1.0, alpha) * std::sqrt(1.0 + (alpha * alpha))) -
                         (alpha * root_rho)));
  const double sigma = alpha * m;
  return {.a = (v_tilde * t) - (b * sigma * root_rho), .b = b, .rho = rho, .m = m, .sigma = sigma};
}

// GJ section 4: the SSVI slice w = theta/2 (1 + rho phi k + sqrt((phi k + rho)^2 + 1 - rho^2)) as
// raw SVI, with GJ eq. (4.5)'s power law phi(theta) = eta / (theta^gamma (1 + theta)^(1 - gamma)).
SviParams FromSsvi(double theta, double rho, double eta, double gamma) {
  const double phi = eta / (std::pow(theta, gamma) * std::pow(1.0 + theta, 1.0 - gamma));
  const double root_rho = std::sqrt(1.0 - (rho * rho));
  return {.a = 0.5 * theta * (1.0 - (rho * rho)),
          .b = 0.5 * theta * phi,
          .rho = rho,
          .m = -rho / phi,
          .sigma = root_rho / phi};
}

std::size_t Count(const std::vector<Violation>& violations, ViolationKind kind) {
  std::size_t n = 0;
  for (const Violation& v : violations) n += v.kind == kind ? 1 : 0;
  return n;
}

const Violation* Find(const std::vector<Violation>& violations, ViolationKind kind) {
  for (const Violation& v : violations) {
    if (v.kind == kind) return &v;
  }
  return nullptr;
}

std::string Describe(const std::vector<Violation>& violations) {
  std::string out;
  for (const Violation& v : violations) {
    std::array<char, 200> line{};
    std::snprintf(line.data(), line.size(), "%s k [%g, %g] worst %g value %.6g bound %.6g n %zu\n",
                  KindName(v.kind), v.k_begin, v.k_end, v.k_worst, v.value, v.bound, v.points);
    out += line.data();
  }
  return out;
}

// SVI-3. GJ Example 3.1: Vogt's slice has negative g, so a butterfly arbitrage, near k = 0.9
// (GJ Figure 1), and MM section 5.5.1 classify it type 3 with the numbers below.
TEST(Butterfly, VogtIsFlaggedWhereGJAndMMFindItsArbitrage) {
  const ButterflyReport report = CheckButterfly(kVogt);
  ASSERT_EQ(Count(report.violations, ViolationKind::kDensity), 1U) << Describe(report.violations);
  const Violation& density = *Find(report.violations, ViolationKind::kDensity);
  EXPECT_LT(density.k_begin, density.k_worst);
  EXPECT_LT(density.k_worst, density.k_end);
  EXPECT_GT(density.points, 10U);
  EXPECT_NEAR(density.k_worst, 0.879, 0.01);
  EXPECT_LT(density.value, -0.03);
  EXPECT_GT(density.value, -0.04);
  EXPECT_EQ(density.value, report.min_g);
  EXPECT_LT(density.bound, 0.0);
  EXPECT_GT(density.bound, -1e-12);
  // The wings are moderate (b (1 + rho) = 0.17), so only the density and the classifier fail.
  EXPECT_EQ(Count(report.violations, ViolationKind::kDPlusLimit), 0U);
  EXPECT_EQ(Count(report.violations, ViolationKind::kRightWing), 0U);
  EXPECT_EQ(Count(report.violations, ViolationKind::kLeftWing), 0U);
  ASSERT_EQ(Count(report.violations, ViolationKind::kMartiniMingone), 1U);
  EXPECT_EQ(Find(report.violations, ViolationKind::kMartiniMingone)->value, 3.0);

  const MartiniMingone& mm = report.classifier;
  EXPECT_EQ(mm.verdict, MmVerdict::kType3);
  EXPECT_NEAR(mm.alpha, -0.09872, kPublished);
  EXPECT_NEAR(mm.mu, 0.86347, kPublished);
  EXPECT_NEAR(mm.mu_lower, -0.72407, kPublished);
  EXPECT_NEAR(mm.mu_upper, 0.82939, kPublished);
  EXPECT_NEAR(mm.fukasawa_threshold, -0.12663, kPublished);
  // mu is above the interval, so the violation is placed at l+, right of the minimum of w.
  EXPECT_EQ(Find(report.violations, ViolationKind::kMartiniMingone)->bound, mm.mu_upper);
  std::printf("Vogt: %s", Describe(report.violations).c_str());
}

// SVI-4: b (1 + rho) = 2.25 breaks Lee's moment bound 2, which QuantLib's bound 4 lets through.
TEST(Butterfly, SlopeTwoPointTwoFiveIsRejectedAtTwoAndPassedByFour) {
  const SviParams steep{.a = 0.02, .b = 1.5, .rho = 0.5, .m = 0.0, .sigma = 0.2};
  const std::vector<Violation> at_two = CheckWings(steep);
  ASSERT_EQ(at_two.size(), 1U);
  EXPECT_EQ(at_two[0].kind, ViolationKind::kRightWing);
  EXPECT_EQ(at_two[0].value, 2.25);
  EXPECT_EQ(at_two[0].bound, 2.0);
  EXPECT_EQ(at_two[0].k_worst, kInf);
  EXPECT_TRUE(CheckWings(steep, 4.0).empty());

  const ButterflyReport report = CheckButterfly(steep);
  EXPECT_EQ(Count(report.violations, ViolationKind::kRightWing), 1U);
  EXPECT_EQ(Count(report.violations, ViolationKind::kLeftWing), 0U);
  EXPECT_EQ(Count(report.violations, ViolationKind::kDPlusLimit), 1U);
  EXPECT_EQ(report.classifier.verdict, MmVerdict::kType1);
  // With bound 4 the wing check is silent, but d+ and the classifier still reject the slice.
  const ButterflyReport lax = CheckButterfly(steep, {}, 4.0);
  EXPECT_EQ(Count(lax.violations, ViolationKind::kRightWing), 0U);
  EXPECT_EQ(Count(lax.violations, ViolationKind::kDPlusLimit), 1U);
  EXPECT_EQ(Count(lax.violations, ViolationKind::kMartiniMingone), 1U);
  std::printf("slope 2.25: %s", Describe(report.violations).c_str());

  // At exactly 2, d+ tends to 0 (MM Lemma 2.1), so GJ Lemma 2.2's limit fails; below, it holds.
  EXPECT_EQ(CheckDPlusLimit({.a = 0.02, .b = 2.0, .rho = 0.0, .m = 0.0, .sigma = 0.2}).size(), 1U);
  EXPECT_TRUE(CheckDPlusLimit({.a = 0.02, .b = 1.99, .rho = 0.0, .m = 0.0, .sigma = 0.2}).empty());
  const std::vector<Violation> left =
      CheckWings({.a = 0.02, .b = 1.5, .rho = -0.5, .m = 0.0, .sigma = 0.2});
  ASSERT_EQ(left.size(), 1U);
  EXPECT_EQ(left[0].kind, ViolationKind::kLeftWing);
  EXPECT_EQ(left[0].k_worst, -kInf);
}

TEST(Butterfly, MmTable1SlicesPassEveryCheck) {
  for (std::size_t i = 0; i < kMmTable1.size(); ++i) {
    const ButterflyReport report = CheckButterfly(kMmTable1[i]);
    EXPECT_TRUE(report.violations.empty()) << "row " << i << "\n" << Describe(report.violations);
    EXPECT_EQ(report.classifier.verdict, MmVerdict::kNoArbitrage) << "row " << i;
    EXPECT_GT(report.min_g, 0.0) << "row " << i;
    std::printf("MM Table 1 row %zu: min g %.6g at k %.4g, sigma* %.6g\n", i, report.min_g,
                report.k_min_g, report.classifier.sigma_star);
  }
}

// GJ Example 5.1: the JW slice with c and v~ replaced by c' = p + 2 psi and v~' = v 4 p c' /
// (p + c')^2, which GJ show is free of butterfly arbitrage.
TEST(Butterfly, GatheralJacquierExample51RepairedSlicePasses) {
  constexpr double kV = 0.01742625;
  constexpr double kPsi = -0.1752111;
  constexpr double kP = 0.6997381;
  // The conversion first, on Vogt's own JW parameters (GJ Example 5.1), back to eq. (3.8).
  const SviParams vogt = FromJumpWings(kV, kPsi, kP, 1.316798, 0.0116249, 1.0);
  EXPECT_NEAR(vogt.a, kVogt.a, 1e-4);
  EXPECT_NEAR(vogt.b, kVogt.b, 1e-4);
  EXPECT_NEAR(vogt.rho, kVogt.rho, 1e-4);
  EXPECT_NEAR(vogt.m, kVogt.m, 1e-4);
  EXPECT_NEAR(vogt.sigma, kVogt.sigma, 1e-4);

  const double c = kP + (2.0 * kPsi);
  const double v_tilde = kV * 4.0 * kP * c / ((kP + c) * (kP + c));
  // GJ print p and psi to seven decimals, so p + 2 psi from them is good to 1.5e-7; GJ's own c'
  // (0.3493158), presumably from unrounded values, is 1e-7 below it.
  EXPECT_NEAR(c, 0.3493158, 2e-7);
  EXPECT_NEAR(v_tilde, 0.01548182, 1e-8);
  const SviParams repaired = FromJumpWings(kV, kPsi, kP, c, v_tilde, 1.0);
  const ButterflyReport report = CheckButterfly(repaired);
  EXPECT_TRUE(report.violations.empty()) << Describe(report.violations);
  EXPECT_EQ(report.classifier.verdict, MmVerdict::kNoArbitrage);
  EXPECT_GT(report.min_g, 0.0);
  std::printf("GJ 5.1 repaired (%.10g, %.10g, %.10g, %.10g, %.10g): min g %.6g at k %.4g\n",
              repaired.a, repaired.b, repaired.rho, repaired.m, repaired.sigma, report.min_g,
              report.k_min_g);
}

// GJ Remark 4.4 (eq. (4.5) with gamma = 1/2): SSVI with eta (1 + |rho|) <= 2 is free of static
// arbitrage, so every slice passes the butterfly checks and every later slice the calendar check.
TEST(Ssvi, PowerLawSurfacesPassButterflyAndCalendar) {
  constexpr std::array<double, 7> kThetas{0.0004, 0.004, 0.01, 0.04, 0.1, 0.4, 1.0};
  struct Surface {
    double rho;
    double eta;
  };
  constexpr std::array<Surface, 3> kSurfaces{
      {{.rho = -0.7, .eta = 1.1}, {.rho = 0.3, .eta = 2.0 / 1.3}, {.rho = 0.0, .eta = 2.0}}};
  for (const Surface& s : kSurfaces) {
    for (std::size_t i = 0; i < kThetas.size(); ++i) {
      const SviParams slice = FromSsvi(kThetas[i], s.rho, s.eta, 0.5);
      const ButterflyReport report = CheckButterfly(slice);
      EXPECT_TRUE(report.violations.empty()) << "rho " << s.rho << " theta " << kThetas[i] << "\n"
                                             << Describe(report.violations);
      EXPECT_EQ(report.classifier.verdict, MmVerdict::kNoArbitrage)
          << "rho " << s.rho << " theta " << kThetas[i] << " "
          << MmVerdictName(report.classifier.verdict);
      if (i == 0) continue;
      const CalendarReport calendar =
          CheckCalendar(FromSsvi(kThetas[i - 1], s.rho, s.eta, 0.5), slice);
      EXPECT_TRUE(calendar.violations.empty()) << "rho " << s.rho << " theta " << kThetas[i] << "\n"
                                               << Describe(calendar.violations);
      EXPECT_GT(calendar.min_gap, 0.0);
    }
  }
}

TEST(Butterfly, InvalidAndNonPositiveSlicesAreReportedNotEvaluated) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::array<SviParams, 6> invalid{{
      {.a = nan, .b = 0.1, .rho = 0.0, .m = 0.0, .sigma = 0.1},
      {.a = 0.04, .b = -0.1, .rho = 0.0, .m = 0.0, .sigma = 0.1},
      {.a = 0.04, .b = 0.1, .rho = 1.5, .m = 0.0, .sigma = 0.1},
      {.a = 0.04, .b = 0.1, .rho = 0.0, .m = 0.0, .sigma = 0.0},
      {.a = 0.04, .b = 0.1, .rho = 0.0, .m = kInf, .sigma = 0.1},
      {.a = 0.04, .b = 0.1, .rho = 0.0, .m = 2e4, .sigma = 0.1},
  }};
  for (const SviParams& p : invalid) {
    const ButterflyReport report = CheckButterfly(p);
    ASSERT_EQ(report.violations.size(), 1U) << Describe(report.violations);
    EXPECT_EQ(report.violations[0].kind, ViolationKind::kInvalidParameters);
    EXPECT_EQ(report.classifier.verdict, MmVerdict::kInvalidParameters);
    EXPECT_EQ(report.grid_points, 0U);
    EXPECT_TRUE(std::isnan(report.min_g));
  }
  // min w = a + b sigma sqrt(1 - rho^2) = -0.1 + 0.008 at k* = m - rho sigma / sqrt(1 - rho^2).
  const SviParams negative{.a = -0.1, .b = 0.1, .rho = 0.6, .m = 0.0, .sigma = 0.1};
  const ButterflyReport report = CheckButterfly(negative);
  ASSERT_EQ(report.violations.size(), 1U);
  EXPECT_EQ(report.violations[0].kind, ViolationKind::kNonPositiveVariance);
  EXPECT_NEAR(report.violations[0].value, -0.092, 1e-15);
  EXPECT_NEAR(report.violations[0].k_worst, -0.075, 1e-15);
  EXPECT_EQ(report.classifier.verdict, MmVerdict::kNonPositiveVariance);
  // |rho| = 1 with a < 0: w falls to a at the far wing.
  const std::vector<Violation> edge =
      CheckParameters({.a = -0.01, .b = 0.1, .rho = -1.0, .m = 0.0, .sigma = 0.1});
  ASSERT_EQ(edge.size(), 1U);
  EXPECT_EQ(edge[0].kind, ViolationKind::kNonPositiveVariance);
  EXPECT_EQ(edge[0].k_worst, kInf);
}

// k -> -k maps a raw slice to (a, b, -rho, -m, sigma) and g(k) to g(-k), so the verdict and the
// grid's minimum of g must not change. That is how rho = +1 is decided.
TEST(MartiniMingone, VerdictIsSymmetricUnderReflection) {
  std::vector<SviParams> slices(kMmTable1.begin(), kMmTable1.end());
  slices.push_back(kVogt);
  slices.push_back({.a = 0.0, .b = 0.5, .rho = -1.0, .m = 0.1, .sigma = 0.2});
  slices.push_back({.a = 0.01, .b = 0.9, .rho = -1.0, .m = -0.3, .sigma = 0.3});
  slices.push_back({.a = 0.02, .b = 1.5, .rho = 0.5, .m = 0.0, .sigma = 0.2});
  for (const SviParams& p : slices) {
    const SviParams mirror{.a = p.a, .b = p.b, .rho = -p.rho, .m = -p.m, .sigma = p.sigma};
    const MartiniMingone direct = ClassifyMartiniMingone(p);
    const MartiniMingone reflected = ClassifyMartiniMingone(mirror);
    EXPECT_EQ(direct.verdict, reflected.verdict)
        << MmVerdictName(direct.verdict) << " vs " << MmVerdictName(reflected.verdict);
    EXPECT_NE(direct.verdict, MmVerdict::kUndecided);
    for (const double k : {-1.3, -0.2, 0.0, 0.4, 2.1}) {
      EXPECT_NEAR(Durrleman(p, k), Durrleman(mirror, -k), 1e-13);
    }
  }
}

// MM Theorem 6.2: with alpha and mu fixed, the slice is free of butterfly arbitrage exactly when
// sigma >= sigma*. Where a Table 1 row has sigma* > 0, the slice at sigma* / 2 must be type 4, and
// the grid must find its negative g independently; at 2 sigma* both must pass.
TEST(MartiniMingone, TypeFourAgreesWithTheGridOnBothSidesOfSigmaStar) {
  std::size_t tried = 0;
  for (const SviParams& row : kMmTable1) {
    const MartiniMingone mm = ClassifyMartiniMingone(row);
    if (!(mm.sigma_star > 0.0)) continue;
    ++tried;
    for (const double scale : {0.5, 2.0}) {
      const double sigma = scale * mm.sigma_star;
      const SviParams p{
          .a = mm.alpha * sigma, .b = row.b, .rho = row.rho, .m = mm.mu * sigma, .sigma = sigma};
      const ButterflyReport report = CheckButterfly(p);
      if (scale < 1.0) {
        EXPECT_EQ(report.classifier.verdict, MmVerdict::kType4);
        // One region or two: sigma* binds on one side, but the other can fail as well.
        EXPECT_GE(Count(report.violations, ViolationKind::kDensity), 1U)
            << Describe(report.violations);
      } else {
        EXPECT_EQ(report.classifier.verdict, MmVerdict::kNoArbitrage);
        EXPECT_TRUE(report.violations.empty()) << Describe(report.violations);
      }
      std::printf("sigma* %.6g, sigma %.6g: %s, min g %.6g\n", mm.sigma_star, sigma,
                  MmVerdictName(report.classifier.verdict), report.min_g);
    }
  }
  EXPECT_GT(tried, 0U);
}

// SVI-5, numerical by necessity: no necessary and sufficient condition on two raw SVI slices is
// known (MM, conclusion). Three constructed crossing pairs, each caught by the part of the check
// built for it, and the same pair the right way round passes.
constexpr SviParams kEarlier{.a = 0.04, .b = 0.2, .rho = -0.3, .m = 0.0, .sigma = 0.1};

TEST(Calendar, CrossingNearTheMoneyIsCaughtByTheGrid) {
  // Same wings, higher intercept, but a sharper bottom: w(0) is 0.055 against 0.06.
  const SviParams later{.a = 0.045, .b = 0.2, .rho = -0.3, .m = 0.0, .sigma = 0.05};
  const CalendarReport report = CheckCalendar(kEarlier, later);
  ASSERT_EQ(report.violations.size(), 1U) << Describe(report.violations);
  const Violation& v = report.violations[0];
  EXPECT_EQ(v.kind, ViolationKind::kCalendarCrossing);
  EXPECT_LT(v.k_begin, 0.0);
  EXPECT_GT(v.k_end, 0.0);
  EXPECT_NEAR(v.value, -0.005, 1e-3);
  EXPECT_EQ(v.value, report.min_gap);
  EXPECT_TRUE(CheckCalendarWings(kEarlier, later).empty());
  std::printf("middle crossing: %s", Describe(report.violations).c_str());
}

TEST(Calendar, CrossingInBothWingsIsCaughtByGridAndAsymptotes) {
  // Higher at the money, flatter wings: crosses near k = 0.57 and k = -0.31.
  const SviParams later{.a = 0.06, .b = 0.15, .rho = -0.3, .m = 0.0, .sigma = 0.1};
  const CalendarReport report = CheckCalendar(kEarlier, later);
  EXPECT_EQ(Count(report.violations, ViolationKind::kCalendarCrossing), 2U)
      << Describe(report.violations);
  ASSERT_EQ(Count(report.violations, ViolationKind::kCalendarRightWing), 1U);
  ASSERT_EQ(Count(report.violations, ViolationKind::kCalendarLeftWing), 1U);
  const Violation& right = *Find(report.violations, ViolationKind::kCalendarRightWing);
  EXPECT_EQ(right.bound, 1.0);  // decided by the slope
  EXPECT_NEAR(right.value, -0.05 * 0.7, 1e-15);
  EXPECT_EQ(right.k_worst, kInf);
  std::printf("wing crossing: %s", Describe(report.violations).c_str());
}

TEST(Calendar, CrossingPastTheGridIsCaughtOnlyByTheAsymptotes) {
  // Wing slopes lower by one part in a thousand: the crossings are near k = 71 and k = -38, far
  // outside the grid, where only the asymptotic comparison sees them.
  const SviParams later{.a = 0.05, .b = 0.2 * (1.0 - 1e-3), .rho = -0.3, .m = 0.0, .sigma = 0.1};
  const CalendarReport report = CheckCalendar(kEarlier, later);
  EXPECT_EQ(Count(report.violations, ViolationKind::kCalendarCrossing), 0U);
  EXPECT_GT(report.min_gap, 0.0);
  EXPECT_EQ(Count(report.violations, ViolationKind::kCalendarRightWing), 1U)
      << Describe(report.violations);
  EXPECT_EQ(Count(report.violations, ViolationKind::kCalendarLeftWing), 1U);
  // Same slopes and intercept, smaller 1/|k| term: decided at the third order.
  const SviParams thinner{.a = 0.04, .b = 0.2, .rho = -0.3, .m = 0.0, .sigma = 0.09};
  const std::vector<Violation> wings = CheckCalendarWings(kEarlier, thinner);
  ASSERT_EQ(wings.size(), 2U) << Describe(wings);
  EXPECT_EQ(wings[0].bound, -1.0);
  EXPECT_EQ(wings[1].bound, -1.0);
}

TEST(Calendar, OrderedAndIdenticalPairsPass) {
  const std::array<SviParams, 3> laters{{
      kEarlier,
      {.a = 0.05, .b = 0.2, .rho = -0.3, .m = 0.0, .sigma = 0.1},
      {.a = 0.04, .b = 0.25, .rho = -0.3, .m = 0.0, .sigma = 0.1},
  }};
  for (const SviParams& later : laters) {
    const CalendarReport report = CheckCalendar(kEarlier, later);
    EXPECT_TRUE(report.violations.empty()) << Describe(report.violations);
    EXPECT_GT(report.grid_points, 6000U);
  }
  // The other way round, each strict pair is a violation.
  EXPECT_FALSE(CheckCalendar(laters[1], kEarlier).violations.empty());
  EXPECT_FALSE(CheckCalendar(laters[2], kEarlier).violations.empty());
}

TEST(Grid, HoldsTheStatedPartsSortedAndUnique) {
  const GridSpec spec;
  const std::vector<double> grid = MakeGrid(spec, &kVogt, 1);
  // 6001 points on [-3, 3] and 8001 on m + sigma [-40, 40], less the ones they share.
  EXPECT_GT(grid.size(), 13000U);
  EXPECT_LE(grid.size(), 14002U);
  EXPECT_DOUBLE_EQ(grid.front(), kVogt.m - (40.0 * kVogt.sigma));
  EXPECT_DOUBLE_EQ(grid.back(), kVogt.m + (40.0 * kVogt.sigma));
  for (std::size_t i = 1; i < grid.size(); ++i) {
    ASSERT_LT(grid[i - 1], grid[i]);
  }
  EXPECT_TRUE(MakeGrid({.k_half_width = 1.0, .k_step = 0.0}, nullptr, 0).empty());
  EXPECT_LT(DensityTolerance(kVogt, 0.879), 1e-13);
  EXPECT_GT(DensityTolerance(kVogt, 0.879), 0.0);
}

}  // namespace
}  // namespace aadp::svi
