// Accuracy and edge contract of pricer::NormalCdf, Cody's erfc on the pricer's own exp.
//
// Target, stated in src/pricer/normal_cdf.hpp before the table was measured: erfc(y) / 2 within
// 4 ULP of the exact value at the y the kernel computes, wherever the result is normal; below
// that, within 2 units of the smallest subnormal. The reference is the committed decimal table,
// normal_cdf_values.inc, which tests/pricer/normal_cdf_reference.py regenerates under ctest.
//
// The same table measures libm's erfc through src/rng/normal.hpp, the normal CDF the frozen
// reference kernels use. The path-by-path bound in path_agreement_test.cpp assumes it is within
// kLibmPremiseUlp, stated here before measuring; this test fails if it is not.
#include "pricer/normal_cdf.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>
#include <utility>

#include "math/reference_table.hpp"
#include "rng/normal.hpp"

namespace aadp::pricer {
namespace {

struct NormalCdfPoint {
  constexpr NormalCdfPoint(double x_value, double y_value, double hi_value, double lo_value,
                           double phi_hi_value, double phi_lo_value, int domain_index)
      : x(x_value),
        y(y_value),
        hi(hi_value),
        lo(lo_value),
        phi_hi(phi_hi_value),
        phi_lo(phi_lo_value),
        domain(domain_index) {}
  double x;
  double y;
  double hi;
  double lo;
  double phi_hi;
  double phi_lo;
  int domain;
};

#include "normal_cdf_values.inc"  // IWYU pragma: keep

constexpr double kTargetUlp = 4.0;
constexpr double kLibmPremiseUlp = 4.0;
constexpr double kSubnormal = 0x1p-1074;
constexpr double kSubnormalTargetUnits = 2.0;
constexpr std::array<const char*, 4> kDomains = {"|y| <= 0.46875", "0.46875 < |y| <= 4",
                                                 "|y| > 4, normal result", "subnormal result"};

struct Worst {
  double error = 0.0;
  double at = 0.0;
  int count = 0;
  void Add(double e, double x) {
    ++count;
    if (!(e <= error)) {
      error = e;
      at = x;
    }
  }
};

double ErrorOf(double value, const NormalCdfPoint& p) {
  if (p.domain == 3) {
    const double abs_error = std::fabs((value - p.hi) - p.lo);
    return std::isnan(value) ? std::numeric_limits<double>::infinity() : abs_error / kSubnormal;
  }
  return math::testing::UlpError(value, p.hi, p.lo);
}

std::array<Worst, 4> Measure(double (*cdf)(double)) {
  std::array<Worst, 4> worst{};
  for (const NormalCdfPoint& p : kNormalCdfReference) {
    worst[static_cast<std::size_t>(p.domain)].Add(ErrorOf(cdf(p.x), p), p.x);
  }
  return worst;
}

void Report(const char* kernel, const std::array<Worst, 4>& worst) {
  for (std::size_t d = 0; d < worst.size(); ++d) {
    std::printf("measured max error, %s, %s: %.4f %s at x = %a, %d points\n", kernel, kDomains[d],
                worst[d].error, d == 3 ? "subnormal units" : "ULP", worst[d].at, worst[d].count);
  }
}

double OwnCdf(double x) { return NormalCdf(x); }
double LibmCdf(double x) { return aadp::normal_cdf(x); }

TEST(NormalCdf, TableMatchesTheKernelsArgument) {
  // The table's y must be the double the kernel divides out, or it measures the wrong thing.
  for (const NormalCdfPoint& p : kNormalCdfReference) {
    ASSERT_EQ(std::bit_cast<std::uint64_t>(-p.x / std::numbers::sqrt2),
              std::bit_cast<std::uint64_t>(p.y))
        << p.x;
  }
  for (std::size_t d = 0; d < kDomains.size(); ++d) {
    int count = 0;
    for (const NormalCdfPoint& p : kNormalCdfReference) {
      count += std::cmp_equal(p.domain, d) ? 1 : 0;
    }
    EXPECT_GE(count, 20) << kDomains[d];
  }
}

TEST(NormalCdf, MatchesTheTableInEveryDomain) {
  const auto worst = Measure(&OwnCdf);
  Report("pricer::NormalCdf", worst);
  for (std::size_t d = 0; d < 3; ++d) EXPECT_LE(worst[d].error, kTargetUlp) << kDomains[d];
  EXPECT_LE(worst[3].error, kSubnormalTargetUnits) << kDomains[3];
}

TEST(NormalCdf, LibmReferenceIsWithinTheBoundsPremise) {
  const auto worst = Measure(&LibmCdf);
  Report("libm erfc via src/rng/normal.hpp", worst);
  for (std::size_t d = 0; d < 3; ++d) EXPECT_LE(worst[d].error, kLibmPremiseUlp) << kDomains[d];
}

// Against Phi at the unrounded x, y carries a relative error up to 2^-52: the rounding of sqrt 2
// and of the division. erfc(y) has relative slope about 2 y, so that adds up to 4 y^2 2^-53, which
// the reference shares. Reported, and bounded by the kernel target plus that term.
TEST(NormalCdf, ErrorAgainstPhiOfTheExactArgument) {
  Worst worst;
  for (const NormalCdfPoint& p : kNormalCdfReference) {
    if (p.domain == 3) continue;
    const double value = NormalCdf(p.x);
    const double relative = std::fabs((value - p.phi_hi) - p.phi_lo) / p.phi_hi;
    const double allowance = (kTargetUlp * 0x1p-52) + ((4.0 * p.y * p.y + 2.0) * 0x1p-53);
    worst.Add(relative / allowance, p.x);
    EXPECT_LE(relative, allowance) << p.x;
  }
  std::printf("measured max error against Phi(x), relative to the allowance: %.4f at x = %a\n",
              worst.error, worst.at);
}

TEST(NormalCdf, EdgeContract) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(std::bit_cast<std::uint64_t>(NormalCdf(nan)), 0x7ff8000000000000U);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(NormalCdf(-nan)), 0x7ff8000000000000U);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(NormalCdf(-inf)), 0U);
  EXPECT_EQ(NormalCdf(inf), 1.0);
  EXPECT_EQ(NormalCdf(0.0), 0.5);
  EXPECT_EQ(NormalCdf(-0.0), 0.5);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(NormalCdf(-40.0)), 0U);
  EXPECT_EQ(NormalCdf(40.0), 1.0);
}

// Non-decreasing on a dense sweep of the whole range, at 2^-10, where Phi moves by far more than
// its error. Ulp by ulp it cannot be: near |y| = 0.46875 Phi moves by under one ULP per input ULP,
// so roundings of a few ULP make steps down, and a join between two separate fits adds its own.
// So ulp by ulp across each join a step down may be at most twice the accuracy target, which two
// values each within the target imply, and the largest is reported.
TEST(NormalCdf, IsMonotone) {
  double previous = NormalCdf(-39.0);
  // An integer counter: every x is a multiple of 2^-10, exact in a double.
  for (int i = 0; i <= 48 * 1024; ++i) {
    const double x = -39.0 + (i * 0x1p-10);
    const double value = NormalCdf(x);
    ASSERT_GE(value, previous) << x;
    previous = value;
  }
  for (const double join : {0.46875, 4.0}) {
    for (const double sign : {1.0, -1.0}) {
      double x = -sign * join * std::numbers::sqrt2;
      for (int i = 0; i < 64; ++i) x = std::nextafter(x, -1.0e9);
      double below = NormalCdf(x);
      double worst_step = 0.0;
      for (int i = 0; i < 128; ++i) {
        x = std::nextafter(x, 1.0e9);
        const double value = NormalCdf(x);
        const double step = (below - value) / (std::nextafter(below, 2.0) - below);
        worst_step = std::fmax(worst_step, step);
        EXPECT_LE(step, 2.0 * kTargetUlp) << x;
        below = value;
      }
      std::printf("measured largest step down, ulp by ulp across |y| = %g, x %s 0: %.1f ULP\n",
                  join, sign > 0 ? "<" : ">", worst_step);
    }
  }
}

// Each side within 4 ULP of values below 1, plus the sum's rounding: 8 units of 2^-53 at most.
TEST(NormalCdf, Symmetry) {
  for (int i = 0; i <= 9 * 256; ++i) {
    const double x = i * 0x1p-8;
    EXPECT_LE(std::fabs((NormalCdf(x) + NormalCdf(-x)) - 1.0), 0x1p-50) << x;
  }
}

}  // namespace
}  // namespace aadp::pricer
