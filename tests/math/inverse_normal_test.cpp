// AS241 against the high-precision inverse normal in the committed table, for both the production
// kernel (math::inverse_normal) and the frozen reference (src/rng/normal.hpp). This separates
// AS241's own error from the error of any reference: the table's values come from Newton on a
// 70-digit Phi (tools/math_reference/generate.py) and are exact to about 1e-32 relative.
//
// Target, stated in src/math/inverse_normal.hpp before measuring: 1.0e-15 maximum relative error.
// BENCHMARKS.md RNG-3 is 6.0e-16, and each test prints whether the measurement meets it. RNG-3
// is reported, never asserted, because it was measured on other arithmetic.
#include "math/inverse_normal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

#include "reference_table.hpp"
#include "rng/normal.hpp"
#include "rng/philox.hpp"

namespace aadp::math::testing {
namespace {

namespace production = inverse_normal_detail;
namespace reference = ppnd16_detail;
static_assert(production::kCentralNum == reference::kCentralNum);
static_assert(production::kCentralDen == reference::kCentralDen);
static_assert(production::kMiddleNum == reference::kMiddleNum);
static_assert(production::kMiddleDen == reference::kMiddleDen);
static_assert(production::kTailNum == reference::kTailNum);
static_assert(production::kTailDen == reference::kTailDen);

constexpr double kTargetRelative = 1.0e-15;
constexpr double kRng3 = 6.0e-16;
// Both kernels are within kTargetRelative of the truth, so they are within twice that of each
// other. In the tails the only difference is log: 1 ULP of log moves r by about half an ULP and
// x by about as much relative, so the measured difference should sit far below this.
constexpr double kTailAgreement = 2.0 * kTargetRelative;
// Development seed, not 1729 or 271828.
constexpr std::uint64_t kSeed = 0x696e766e6f726d00U;

struct Worst {
  double error = 0.0;
  double at = 0.0;
  std::size_t count = 0;
  void Add(double e, double p) {
    ++count;
    if (!(e <= error)) {
      error = e;
      at = p;
    }
  }
};

std::array<Worst, kInverseNormalReferenceDomains.size()> MeasureAgainstTable(
    double (*kernel)(double)) {
  std::array<Worst, kInverseNormalReferenceDomains.size()> worst{};
  for (const ReferencePoint& point : kInverseNormalReference) {
    worst[static_cast<std::size_t>(point.domain)].Add(
        RelativeError(kernel(point.input), point.hi, point.lo), point.input);
  }
  return worst;
}

// A string_view, not a const char*: g++ 14 at -O3 calls the pointer possibly null in the printf
// (-Wformat-overflow) though every caller passes a literal.
void ReportAndCheck(std::string_view name, double (*kernel)(double)) {
  const auto worst = MeasureAgainstTable(kernel);
  const int name_length = static_cast<int>(name.size());
  Worst overall;
  for (std::size_t d = 0; d < worst.size(); ++d) {
    std::printf("measured max relative error, %.*s, %s: %.3e at p = %a (%.6e), %zu points\n",
                name_length, name.data(), kInverseNormalReferenceDomains[d], worst[d].error,
                worst[d].at, worst[d].at, worst[d].count);
    EXPECT_GT(worst[d].count, 0U);
    EXPECT_LE(worst[d].error, kTargetRelative) << name << ", " << kInverseNormalReferenceDomains[d];
    if (!(worst[d].error <= overall.error)) overall = worst[d];
  }
  std::printf("RNG-3, %.*s: maximum %.3e at p = %a against the target 6.0e-16: %s\n", name_length,
              name.data(), overall.error, overall.at, overall.error <= kRng3 ? "met" : "missed");
}

TEST(MathInverseNormal, ProductionMatchesTheTable) {
  ReportAndCheck("production math::inverse_normal", &math::inverse_normal);
}

TEST(MathInverseNormal, FrozenReferenceMatchesTheTable) {
  ReportAndCheck("frozen reference src/rng/normal.hpp", &aadp::inverse_normal);
}

// Where Phase 2's test against its erfc-polished reference saw its largest errors, 8.40e-16,
// 8.47e-16 and 7.45e-16. The table holds these exact p, so AS241's own error there is printed.
TEST(MathInverseNormal, Phase2WorstPointsAreSeparated) {
  for (const double p : {0x1.3559c49774256p-1, 0x1.5fe4df4c60464p-33, 0x1.e2aec6f91d1f0p-597}) {
    bool found = false;
    for (const ReferencePoint& point : kInverseNormalReference) {
      if (point.input != p) continue;
      found = true;
      std::printf(
          "Phase 2 point p = %a: exact %a%+a, AS241 reference %a, error %.3e; "
          "production %a, error %.3e\n",
          p, point.hi, point.lo, aadp::inverse_normal(p),
          RelativeError(aadp::inverse_normal(p), point.hi, point.lo), math::inverse_normal(p),
          RelativeError(math::inverse_normal(p), point.hi, point.lo));
      break;
    }
    EXPECT_TRUE(found) << p;
  }
}

// The central branch has no log, and the same operations in the same order.
TEST(MathInverseNormal, CentralRegionIsBitIdenticalToTheReference) {
  std::size_t checked = 0;
  std::size_t differ = 0;
  const auto compare = [&](double p) {
    ++checked;
    if (std::bit_cast<std::uint64_t>(math::inverse_normal(p)) !=
        std::bit_cast<std::uint64_t>(aadp::inverse_normal(p))) {
      ++differ;
      ADD_FAILURE() << "p = " << p;
    }
  };
  for (const ReferencePoint& point : kInverseNormalReference) {
    if (point.domain == 0) compare(point.input);
  }
  constexpr std::size_t kSteps = std::size_t{1} << 20;
  for (std::size_t i = 0; i <= kSteps; ++i) compare(0.075 + 0.85 * static_cast<double>(i) / kSteps);
  std::printf("central region: %zu of %zu differ in any bit\n", differ, checked);
  EXPECT_EQ(differ, 0U);
}

TEST(MathInverseNormal, TailsAgreeWithTheReference) {
  std::size_t checked = 0;
  std::size_t identical = 0;
  double worst = 0.0;
  double worst_p = 0.0;
  const auto compare = [&](double p) {
    const double ours = math::inverse_normal(p);
    const double theirs = aadp::inverse_normal(p);
    ++checked;
    if (std::bit_cast<std::uint64_t>(ours) == std::bit_cast<std::uint64_t>(theirs)) ++identical;
    const double difference = std::fabs(ours - theirs) / std::fabs(theirs);
    if (!(difference <= worst)) {
      worst = difference;
      worst_p = p;
    }
  };
  for (const ReferencePoint& point : kInverseNormalReference) {
    if (point.domain != 0) compare(point.input);
  }
  std::uint64_t state = kSeed;
  for (int i = 0; i < (1 << 20); ++i) {
    state += 0x9e3779b97f4a7c15U;
    const double p = UniformFromBits(state * 0xbf58476d1ce4e5b9U);
    if (std::fabs(p - 0.5) > 0.425) compare(p);
  }
  std::printf(
      "tails: %zu of %zu bit-identical to the reference, max relative difference %.3e at "
      "p = %a, on this host's libm\n",
      identical, checked, worst, worst_p);
  EXPECT_LE(worst, kTailAgreement);
}

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

TEST(MathInverseNormal, EdgeContract) {
  constexpr std::uint64_t kCanonicalNaN = 0x7ff8000000000000U;
  EXPECT_EQ(math::inverse_normal(0.0), -kInf);
  EXPECT_EQ(math::inverse_normal(1.0), kInf);
  EXPECT_EQ(Bits(math::inverse_normal(0.5)), Bits(0.0));
  for (const double p : {-0.0, std::numeric_limits<double>::denorm_min(), 0x1p-1022}) {
    const double x = math::inverse_normal(p);
    EXPECT_TRUE(p == 0.0 ? x == -kInf : std::isfinite(x)) << p;
  }
  for (const double p :
       {-std::numeric_limits<double>::denorm_min(), -1.0, std::nextafter(1.0, 2.0), 2.0, kInf,
        -kInf, std::numeric_limits<double>::quiet_NaN(), -std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::signaling_NaN()}) {
    EXPECT_EQ(Bits(math::inverse_normal(p)), kCanonicalNaN) << p;
  }
}

// Dyadic p only: 1 - p is then exact, so both calls see the same tail.
TEST(MathInverseNormal, IsAntisymmetric) {
  for (const double p : {0x1p-50, 0x1p-40, 0x1p-20, 0.0625, 0.25, 0.4375}) {
    EXPECT_EQ(math::inverse_normal(p), -math::inverse_normal(1.0 - p)) << p;
  }
}

}  // namespace
}  // namespace aadp::math::testing
