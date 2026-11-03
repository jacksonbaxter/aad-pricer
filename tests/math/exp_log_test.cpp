// Accuracy and edge contract of math::exp, math::log and math::sqrt.
//
// Targets, stated in the kernel headers before any measurement and repeated here: exp and log
// below 1 ULP of the exact result for every argument, from an error budget of about 0.8 and 0.9
// ULP; sqrt correctly rounded, 0.5 ULP, because IEEE 754 requires it.
//
// Two references, both more accurate than the kernels by many orders of magnitude:
// - the committed decimal table (reference_values.inc, tools/math_reference/generate.py): random
//   points, reduction boundaries, edges and the GBM working domain, exact to about 1e-16 ULP;
// - double-double exp, log and sqrt (double_double.hpp), about 1e-13 ULP, for the dense sweeps.
//   They are checked against the table first, so a sweep never rests on an unchecked reference.
//
// Each test prints its measured maximum per domain. Outside Release the sweeps run at 1/16 size
// as a smoke test, and say so: the measured numbers come from the Release build.
#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>

#include "double_double.hpp"
#include "math/common.hpp"
#include "math/exp.hpp"
#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "reference_table.hpp"

#ifndef AADP_SMOKE_SIZE
#error "cmake/area_pricer.cmake sets AADP_SMOKE_SIZE"
#endif

namespace aadp::math::testing {
namespace {

constexpr double kExpTargetUlp = 1.0;
constexpr double kLogTargetUlp = 1.0;
constexpr double kSqrtTargetUlp = 0.5;
// The double-double reference against the table: both are near 1e-13 ULP or better, so 1e-9
// leaves room for neither to hide a real error.
constexpr double kReferenceAgreementUlp = 1e-9;
// Development seed, not 1729 or 271828.
constexpr std::uint64_t kSeed = 0x6d61746873656564U;

constexpr std::size_t Size(std::size_t full) { return AADP_SMOKE_SIZE ? full / 16 : full; }

std::uint64_t SplitMix64(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9e3779b97f4a7c15U);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9U;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebU;
  return z ^ (z >> 31);
}

double Uniform(std::uint64_t& state) {
  return static_cast<double>(SplitMix64(state) >> 11) * 0x1p-53;
}

struct Worst {
  double error = 0.0;
  double at = 0.0;
  std::size_t count = 0;
  void Add(double e, double x) {
    ++count;
    if (!(e <= error)) {
      error = e;
      at = x;
    }
  }
};

void Report(const char* kernel, const std::string& domain, const Worst& worst, const char* unit) {
  std::printf("measured max error, %s, %s: %.4f %s at %a, %zu points%s\n", kernel, domain.c_str(),
              worst.error, unit, worst.at, worst.count,
              AADP_SMOKE_SIZE ? " (reduced size, not a result)" : "");
}

template <std::size_t N, std::size_t M>
void CheckTable(const char* kernel, const std::array<ReferencePoint, N>& table,
                const std::array<const char*, M>& domains, double (*kernel_fn)(double),
                double target) {
  std::array<Worst, M> worst{};
  for (const ReferencePoint& point : table) {
    worst[static_cast<std::size_t>(point.domain)].Add(
        UlpError(kernel_fn(point.input), point.hi, point.lo), point.input);
  }
  for (std::size_t d = 0; d < M; ++d) {
    Report(kernel, std::string("table, ") + domains[d], worst[d], "ULP");
    EXPECT_GT(worst[d].count, 0U) << domains[d];
    EXPECT_LT(worst[d].error, target) << kernel << ", " << domains[d];
  }
}

// ---------------------------------------------------------------------------------------------

TEST(MathReference, DoubleDoubleMatchesTheTable) {
  Worst exp_worst;
  for (const ReferencePoint& point : kExpReference) {
    if (!(point.input >= -670.0 && point.input < 709.7)) continue;
    const DoubleDouble dd = ExpDD(point.input);
    exp_worst.Add(std::fabs((dd.hi - point.hi) + (dd.lo - point.lo)) / UlpOfExact(point.hi, 0.0),
                  point.input);
  }
  Worst log_worst;
  for (const ReferencePoint& point : kLogReference) {
    const DoubleDouble dd = LogDD(point.input);
    log_worst.Add(std::fabs((dd.hi - point.hi) + (dd.lo - point.lo)) / UlpOfExact(point.hi, 0.0),
                  point.input);
  }
  Report("double-double exp against the table", "-670 to 709.7", exp_worst, "ULP");
  Report("double-double log against the table", "every point", log_worst, "ULP");
  EXPECT_GT(exp_worst.count, 2000U);
  EXPECT_LT(exp_worst.error, kReferenceAgreementUlp);
  EXPECT_LT(log_worst.error, kReferenceAgreementUlp);
}

TEST(MathExp, MatchesTheTableInEveryDomain) {
  CheckTable("exp", kExpReference, kExpReferenceDomains, &math::exp, kExpTargetUlp);
}

TEST(MathLog, MatchesTheTableInEveryDomain) {
  CheckTable("log", kLogReference, kLogReferenceDomains, &math::log, kLogTargetUlp);
}

// ---------------------------------------------------------------------------------------------

using Visit = std::function<void(double)>;
using Generator = std::function<void(const Visit&)>;

void SweepExp(const char* domain, const Generator& generate) {
  Worst worst;
  generate([&](double x) {
    const DoubleDouble ref = ExpDD(x);
    worst.Add(UlpError(math::exp(x), ref.hi, ref.lo), x);
  });
  Report("exp", domain, worst, "ULP");
  EXPECT_LT(worst.error, kExpTargetUlp) << domain;
}

void SweepLog(const char* domain, const Generator& generate) {
  Worst worst;
  generate([&](double x) {
    const DoubleDouble ref = LogDD(x);
    worst.Add(UlpError(math::log(x), ref.hi, ref.lo), x);
  });
  Report("log", domain, worst, "ULP");
  EXPECT_LT(worst.error, kLogTargetUlp) << domain;
}

void Evenly(double lo, double hi, std::size_t n, const Visit& visit) {
  for (std::size_t i = 0; i <= n; ++i) {
    visit(lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(n));
  }
}

// centre and count doubles either side of it, for centre > 0, stopping at 0 and infinity.
void Around(double centre, int count, const Visit& visit) {
  double x = centre;
  for (int i = 0; i < count && x > 0x1p-1074; ++i) x = std::nextafter(x, 0.0);
  for (int i = 0; i <= 2 * count && x < kInf; ++i) {
    visit(x);
    x = std::nextafter(x, kInf);
  }
}

TEST(MathExp, SweepGbmDomainEvenly) {
  SweepExp("sweep, GBM working domain -1 to 1, evenly spaced",
           [](const Visit& visit) { Evenly(-1.0, 1.0, Size(std::size_t{1} << 21), visit); });
}

TEST(MathExp, SweepWideGbmDomainRandomly) {
  SweepExp("sweep, GBM working domain -20 to 20, random", [](const Visit& visit) {
    std::uint64_t state = kSeed;
    for (std::size_t i = 0; i < Size(std::size_t{1} << 20); ++i) {
      visit(-20.0 + 40.0 * Uniform(state));
    }
  });
}

TEST(MathExp, SweepFullRangeRandomly) {
  SweepExp("sweep, -670 to 709.7, random", [](const Visit& visit) {
    std::uint64_t state = kSeed + 1;
    for (std::size_t i = 0; i < Size(std::size_t{1} << 20); ++i) {
      visit(-670.0 + 1379.7 * Uniform(state));
    }
  });
}

TEST(MathExp, SweepEveryReductionBoundary) {
  SweepExp("sweep, every k from -966 to 1023, 16 ulps around (k + 1/2) ln2 and k ln2",
           [](const Visit& visit) {
             const Visit in_range = [&](double x) {
               if (x >= -670.0 && x < 709.7) visit(x);
             };
             for (int k = -966; k <= 1023; ++k) {
               const auto kd = static_cast<double>(k);
               // Negative centres step through their magnitude, which is what Around expects.
               for (const double centre : {KLn2(kd + 0.5).hi, KLn2(kd).hi}) {
                 if (centre < 0.0) {
                   Around(-centre, 16, [&](double x) { in_range(-x); });
                 } else {
                   Around(centre, 16, in_range);
                 }
               }
             }
           });
}

TEST(MathLog, SweepGbmDomainEvenly) {
  SweepLog("sweep, GBM working domain 0.25 to 4, evenly spaced",
           [](const Visit& visit) { Evenly(0.25, 4.0, Size(std::size_t{1} << 21), visit); });
}

// Inputs from math::exp, so they are the same on every host.
TEST(MathLog, SweepWideGbmDomainLogUniformly) {
  SweepLog("sweep, GBM working domain 1e-3 to 1e3, log-uniform", [](const Visit& visit) {
    std::uint64_t state = kSeed + 2;
    for (std::size_t i = 0; i < Size(std::size_t{1} << 20); ++i) {
      visit(math::exp(-6.907755278982137 + 13.815510557964274 * Uniform(state)));
    }
  });
}

TEST(MathLog, SweepInverseNormalTailsLogUniformly) {
  SweepLog("sweep, inverse-normal tails 2^-53 to 0.075, log-uniform", [](const Visit& visit) {
    std::uint64_t state = kSeed + 3;
    for (std::size_t i = 0; i < Size(std::size_t{1} << 19); ++i) {
      visit(math::exp(-36.7368005696771 + 34.1464 * Uniform(state)));
    }
  });
}

TEST(MathLog, SweepRandomBitPatterns) {
  SweepLog("sweep, random bit patterns, every binade and subnormals", [](const Visit& visit) {
    std::uint64_t state = kSeed + 4;
    for (std::size_t i = 0; i < Size(std::size_t{1} << 20); ++i) {
      const std::uint64_t bits = SplitMix64(state) % 0x7ff0000000000000U;
      if (bits != 0) visit(std::bit_cast<double>(bits));
    }
  });
}

TEST(MathLog, SweepNearOne) {
  SweepLog("sweep, near 1: 1 +- 2^15 ulps, and 0.99 to 1.01", [](const Visit& visit) {
    Around(1.0, 1 << 15, visit);
    Evenly(0.99, 1.01, Size(std::size_t{1} << 20), visit);
  });
}

TEST(MathLog, SweepEveryReductionBoundary) {
  SweepLog("sweep, every binade, 16 ulps around sqrt 2 times 2^e", [](const Visit& visit) {
    for (int e = -1074; e <= 1023; ++e) {
      const double centre = std::ldexp(detail::kSqrt2, e);
      if (centre > 0.0) Around(centre, 16, visit);
    }
  });
}

TEST(MathSqrt, IsCorrectlyRounded) {
  Worst worst;
  std::uint64_t state = kSeed + 5;
  for (std::size_t i = 0; i < Size(std::size_t{1} << 20); ++i) {
    // Exponents 2^-960 to 2^1000, where the double-double residual neither underflows nor
    // overflows. Subnormals are checked through exact scaling below.
    const double x =
        std::ldexp(1.0 + Uniform(state), static_cast<int>(SplitMix64(state) % 1960) - 960);
    const DoubleDouble ref = SqrtDD(x);
    worst.Add(UlpError(math::sqrt(x), ref.hi, ref.lo), x);
  }
  Report("sqrt", "random, 2^-960 to 2^1000", worst, "ULP");
  EXPECT_LE(worst.error, kSqrtTargetUlp + kReferenceAgreementUlp);
  for (std::uint64_t bits = 1; bits < 0x0010000000000000U; bits = bits * 3 + 1) {
    const auto x = std::bit_cast<double>(bits);
    EXPECT_EQ(math::sqrt(x), std::ldexp(math::sqrt(std::ldexp(x, 200)), -100)) << x;
  }
}

// ---------------------------------------------------------------------------------------------

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

constexpr std::uint64_t kCanonicalNaN = 0x7ff8000000000000U;
const std::array<double, 4> kNaNs = {std::numeric_limits<double>::quiet_NaN(),
                                     -std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::signaling_NaN(),
                                     std::bit_cast<double>(std::uint64_t{0xfff0000000000001U})};

TEST(MathExp, EdgeContract) {
  for (const double nan : kNaNs) EXPECT_EQ(Bits(math::exp(nan)), kCanonicalNaN);
  EXPECT_EQ(math::exp(kInf), kInf);
  EXPECT_EQ(Bits(math::exp(-kInf)), Bits(0.0));
  EXPECT_EQ(math::exp(710.0), kInf);
  EXPECT_EQ(math::exp(1e308), kInf);
  EXPECT_EQ(Bits(math::exp(-746.0)), Bits(0.0));
  EXPECT_EQ(Bits(math::exp(-1e308)), Bits(0.0));
  // ln(DBL_MAX) lies between these two doubles.
  EXPECT_TRUE(std::isfinite(math::exp(0x1.62e42fefa39efp+9)));
  EXPECT_EQ(math::exp(0x1.62e42fefa39f0p+9), kInf);
  // Gradual underflow: exp(-745.1) is 0.517 of the smallest subnormal and rounds up to it.
  EXPECT_EQ(math::exp(-745.1), std::numeric_limits<double>::denorm_min());
  EXPECT_TRUE(math::exp(-708.5) > 0.0 && math::exp(-708.5) < 0x1p-1022);
  for (const double x : {0.0, -0.0, std::numeric_limits<double>::denorm_min(),
                         -std::numeric_limits<double>::denorm_min(), 0x1p-1022, 0x1p-60}) {
    EXPECT_EQ(math::exp(x), 1.0) << x;
  }
}

TEST(MathLog, EdgeContract) {
  for (const double nan : kNaNs) EXPECT_EQ(Bits(math::log(nan)), kCanonicalNaN);
  EXPECT_EQ(math::log(0.0), -kInf);
  EXPECT_EQ(math::log(-0.0), -kInf);
  EXPECT_EQ(math::log(kInf), kInf);
  for (const double x : {-kInf, -1.0, -std::numeric_limits<double>::denorm_min(), -1e308}) {
    EXPECT_EQ(Bits(math::log(x)), kCanonicalNaN) << x;
  }
  EXPECT_EQ(Bits(math::log(1.0)), Bits(0.0));
  EXPECT_TRUE(std::isfinite(math::log(std::numeric_limits<double>::denorm_min())));
  EXPECT_TRUE(std::isfinite(math::log(std::numeric_limits<double>::max())));
}

TEST(MathSqrt, EdgeContract) {
  for (const double nan : kNaNs) EXPECT_EQ(Bits(math::sqrt(nan)), kCanonicalNaN);
  for (const double x : {-kInf, -1.0, -std::numeric_limits<double>::denorm_min()}) {
    EXPECT_EQ(Bits(math::sqrt(x)), kCanonicalNaN) << x;
  }
  EXPECT_EQ(Bits(math::sqrt(0.0)), Bits(0.0));
  EXPECT_EQ(Bits(math::sqrt(-0.0)), Bits(-0.0));
  EXPECT_EQ(math::sqrt(kInf), kInf);
  EXPECT_EQ(math::sqrt(4.0), 2.0);
}

}  // namespace
}  // namespace aadp::math::testing
