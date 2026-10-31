// AS241 PPND16 against a reference that shares none of its code: bisection on std::erfc, then
// Halley polish. std::erf replaces erfc next to p = 1/2, where Phi(x) - 1/2 from erfc would
// cancel. In the far tail erfc keeps full relative accuracy, and x changes by about eps / x^2
// for a relative change eps in p, so the reference is no worse there than in the centre.
//
// This is an accuracy check on one host's libm. It is not the RNG-3 benchmark, which needs a
// high-precision reference. Tolerance fixed before measuring: 2e-15 relative, the published
// 6.0e-16 on non-IEEE arithmetic plus a few ULP for this reference's own rounding.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

#include "rng/normal.hpp"

namespace aadp {
namespace {

constexpr double kTolerance = 2e-15;

// Solves Phi(x) = p for p <= 1/2. p - 1/2 is exact in this range.
double ReferenceLowerTail(double p) {
  const double offset = p - 0.5;
  const auto residual_over_pdf = [&](double x) {
    const double residual = std::fabs(offset) < 0.1
                                ? 0.5 * std::erf(x / std::numbers::sqrt2) - offset
                                : 0.5 * std::erfc(-x / std::numbers::sqrt2) - p;
    return residual / normal_pdf(x);
  };
  double lo = -40.0;
  double hi = 0.0;
  for (int i = 0; i < 200 && hi - lo > 1e-3; ++i) {
    const double mid = 0.5 * (lo + hi);
    (0.5 * std::erfc(-mid / std::numbers::sqrt2) < p ? lo : hi) = mid;
  }
  double x = 0.5 * (lo + hi);
  for (int i = 0; i < 12; ++i) {
    const double step = residual_over_pdf(x);
    x -= step / (1.0 + 0.5 * x * step);
  }
  return x;
}

double Reference(double p) { return p <= 0.5 ? ReferenceLowerTail(p) : -ReferenceLowerTail(1 - p); }

double RelativeError(double p) {
  const double reference = Reference(p);
  return std::fabs(inverse_normal(p) - reference) / std::fabs(reference);
}

struct Region {
  const char* name;
  double lo;
  double hi;
  bool log_spaced;
};

// The three branches of PPND16, lower side. r = sqrt(-log p) <= 5 is p >= exp(-25) = 1.4e-11.
constexpr auto kRegions = std::to_array<Region>({
    {.name = "central 0.075 to 0.4999", .lo = 0.075, .hi = 0.4999, .log_spaced = false},
    {.name = "middle 1.4e-11 to 0.075", .lo = 1.5e-11, .hi = 0.075, .log_spaced = true},
    {.name = "far tail 1e-300 to 1.4e-11", .lo = 1e-300, .hi = 1.3e-11, .log_spaced = true},
});

TEST(InverseNormal, MatchesErfcReferenceInEveryRegion) {
  constexpr int kPoints = 4001;
  for (const Region& region : kRegions) {
    double worst = 0.0;
    double worst_p = 0.0;
    for (int i = 0; i < kPoints; ++i) {
      const double t = static_cast<double>(i) / (kPoints - 1);
      const double p = region.log_spaced
                           ? std::exp(std::log(region.lo) * (1 - t) + std::log(region.hi) * t)
                           : region.lo + (region.hi - region.lo) * t;
      for (const double point : {p, 1.0 - p}) {
        if (point == 1.0) continue;
        const double error = RelativeError(point);
        if (error > worst) {
          worst = error;
          worst_p = point;
        }
      }
    }
    std::printf("observed max relative error, %s: %.3e at p = %.6e\n", region.name, worst, worst_p);
    EXPECT_LE(worst, kTolerance) << region.name;
  }
}

// Branch joins, where a wrong coefficient or a wrong test would show first.
TEST(InverseNormal, MatchesReferenceAtBranchBoundaries) {
  const double exp25 = std::exp(-25.0);
  for (const double p : {0.075, 0.925, exp25}) {
    for (int ulps = -4; ulps <= 4; ++ulps) {
      double q = p;
      for (int i = 0; i < std::abs(ulps); ++i) {
        q = std::nextafter(q, ulps < 0 ? 0.0 : 1.0);
      }
      EXPECT_LE(RelativeError(q), kTolerance) << "p = " << q;
    }
  }
}

// Dyadic p only: 1 - p is then exact, so the two calls see the same tail probability.
TEST(InverseNormal, IsAntisymmetric) {
  for (const double p : {0x1p-40, 0x1p-20, 0.0625, 0.25, 0.4375}) {
    EXPECT_EQ(inverse_normal(p), -inverse_normal(1.0 - p)) << p;
  }
}

// Upper values stop at 1: beyond that Phi(x) carries too few digits of 1 - Phi(x) to invert.
TEST(InverseNormal, RoundTripsThroughTheCdf) {
  for (const double x : {-37.0, -12.0, -6.0, -1.0, -1e-3, 0.0, 1e-3, 1.0}) {
    const double p = normal_cdf(x);
    EXPECT_NEAR(inverse_normal(p), x, 1e-13 * std::max(1.0, std::fabs(x))) << x;
  }
}

TEST(InverseNormal, EdgeInputs) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(inverse_normal(0.0), -kInf);
  EXPECT_EQ(inverse_normal(1.0), kInf);
  EXPECT_EQ(inverse_normal(0.5), 0.0);
  EXPECT_TRUE(std::isnan(inverse_normal(-1e-300)));
  EXPECT_TRUE(std::isnan(inverse_normal(std::nextafter(1.0, 2.0))));
  EXPECT_TRUE(std::isnan(inverse_normal(std::numeric_limits<double>::quiet_NaN())));
  EXPECT_TRUE(std::isfinite(inverse_normal(std::numeric_limits<double>::denorm_min())));
}

}  // namespace
}  // namespace aadp
