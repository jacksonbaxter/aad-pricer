// Critical values: NIST/SEMATECH e-Handbook section 7.4.7.3 gives t(.9875, 16) = 2.473. The
// others are BENCHMARKS.md section 10 and research critical_values.py, by a different method
// (numerical integration of the density).
#include "oracle/tolerance.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "oracle/student_t.hpp"

namespace aadp {
namespace {

constexpr double kThreeDecimals = 5e-4;

TEST(StudentT, NistValue) { EXPECT_NEAR(StudentTQuantile(0.9875, 16.0), 2.473, kThreeDecimals); }

TEST(StudentT, BonferroniConstantsForKOf100) {
  EXPECT_NEAR(CriticalValue(0.01, 100, 32), 4.461, kThreeDecimals);
  EXPECT_NEAR(CriticalValue(0.01, 100, 64), 4.155, kThreeDecimals);
  EXPECT_NEAR(CriticalValue(0.01, 100, 128), 4.018, kThreeDecimals);
}

// Exact quantiles exist for nu = 1 (Cauchy) and nu = 2, and they exercise both branches of the
// incomplete beta, so this is a check the printed tables cannot give.
TEST(StudentT, ClosedFormQuantiles) {
  for (double p : {0.6, 0.9, 0.99, 0.9999, 1.0 - 1e-6}) {
    EXPECT_NEAR(StudentTQuantile(p, 1.0), std::tan(std::numbers::pi * (p - 0.5)),
                2e-9 * StudentTQuantile(p, 1.0));
    const double alpha = 4.0 * p * (1.0 - p);
    EXPECT_NEAR(StudentTQuantile(p, 2.0), 2.0 * (p - 0.5) * std::sqrt(2.0 / alpha),
                2e-9 * StudentTQuantile(p, 2.0));
  }
}

TEST(StudentT, ApproachesNormalForLargeNu) {
  EXPECT_NEAR(StudentTQuantile(0.975, 1e7), 1.959964, 1e-5);
}

TEST(StudentT, RejectsBadInput) {
  EXPECT_TRUE(std::isnan(StudentTQuantile(0.4, 10.0)));
  EXPECT_TRUE(std::isnan(StudentTQuantile(1.0, 10.0)));
  EXPECT_TRUE(std::isnan(StudentTQuantile(0.9, 0.0)));
  EXPECT_TRUE(std::isnan(RegularizedIncompleteBeta(1.5, 1.0, 1.0)));
  EXPECT_DOUBLE_EQ(RegularizedIncompleteBeta(0.25, 1.0, 1.0), 0.25);
  EXPECT_TRUE(std::isnan(CriticalValue(0.01, 0, 64)));
  EXPECT_TRUE(std::isnan(CriticalValue(0.01, 100, 1)));
}

TEST(BatchStatistics, MatchesHandComputation) {
  const std::vector<double> means = {1.0, 2.0, 3.0, 4.0};
  const BatchStatistics stats = BatchMeansStatistics(means);
  EXPECT_DOUBLE_EQ(stats.mean, 2.5);
  EXPECT_DOUBLE_EQ(stats.standard_error, std::sqrt(5.0 / 3.0 / 4.0));
  EXPECT_TRUE(std::isnan(BatchMeansStatistics({}).mean));
  const double one = 1.0;
  EXPECT_TRUE(std::isnan(BatchMeansStatistics({&one, 1}).standard_error));
}

// mean 2.5, SE sqrt(5/12) = 0.6455, so critical 2 gives 1.291 + b.
const std::vector<double> kBatches = {1.0, 2.0, 3.0, 4.0};

TEST(Tolerance, PassesWhenBothConditionsHold) {
  const ToleranceResult r =
      CheckAgainstReference(kBatches, 2.5, {.b = 0, .se_ceiling = 1, .critical = 2});
  EXPECT_TRUE(r.passed());
  EXPECT_STREQ(r.failed_condition(), "none");
}

TEST(Tolerance, ConditionOneFailsAlone) {
  const ToleranceResult r =
      CheckAgainstReference(kBatches, 5.0, {.b = 0, .se_ceiling = 1, .critical = 2});
  EXPECT_FALSE(r.passed());
  EXPECT_FALSE(r.within_bound);
  EXPECT_TRUE(r.within_ceiling);
  EXPECT_STREQ(r.failed_condition(), "distance");
}

// A noisy run that sits on the reference still fails. That is the point of condition 2.
TEST(Tolerance, ConditionTwoFailsAlone) {
  const ToleranceResult r =
      CheckAgainstReference(kBatches, 2.5, {.b = 0, .se_ceiling = 0.1, .critical = 2});
  EXPECT_FALSE(r.passed());
  EXPECT_TRUE(r.within_bound);
  EXPECT_FALSE(r.within_ceiling);
  EXPECT_STREQ(r.failed_condition(), "se-ceiling");
}

TEST(Tolerance, BothConditionsFail) {
  const ToleranceResult r =
      CheckAgainstReference(kBatches, 50.0, {.b = 0, .se_ceiling = 0.1, .critical = 2});
  EXPECT_FALSE(r.passed());
  EXPECT_STREQ(r.failed_condition(), "both");
}

TEST(Tolerance, BoundIsCriticalTimesSePlusBAndItIsInclusive) {
  const double se = std::sqrt(5.0 / 12.0);
  const ToleranceResult r = CheckAgainstReference(kBatches, 2.5 + 3.0 * se + 0.25,
                                                  {.b = 0.25, .se_ceiling = 1, .critical = 3});
  EXPECT_DOUBLE_EQ(r.bound, 3.0 * se + 0.25);
  const ToleranceResult wider = CheckAgainstReference(kBatches, 2.5 + 3.0 * se + 0.2501,
                                                      {.b = 0.25, .se_ceiling = 1, .critical = 3});
  EXPECT_FALSE(wider.within_bound);
}

TEST(Tolerance, NanCannotPass) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<double> poisoned = {1.0, nan, 3.0, 4.0};
  const ToleranceResult r =
      CheckAgainstReference(poisoned, 2.5, {.b = 1e9, .se_ceiling = 1e9, .critical = 2});
  EXPECT_FALSE(r.passed());
  EXPECT_STREQ(r.failed_condition(), "both");
  EXPECT_FALSE(
      CheckAgainstReference(kBatches, nan, {.b = 1e9, .se_ceiling = 1e9, .critical = 2}).passed());
  EXPECT_FALSE(
      CheckAgainstReference(kBatches, 2.5, {.b = 0, .se_ceiling = 1, .critical = nan}).passed());
  EXPECT_FALSE(
      CheckAgainstReference(kBatches, 2.5, {.b = 0, .se_ceiling = nan, .critical = 2}).passed());
}

// The difference has SE far below either series, which is why FD rows are paired.
TEST(Tolerance, PairedUsesTheDifferenceSe) {
  const std::vector<double> reference = {1.1, 2.1, 2.9, 4.1};
  const ToleranceResult r =
      CheckPaired(kBatches, reference, {.b = 0, .se_ceiling = 0.1, .critical = 2});
  const std::vector<double> differences = {-0.1, -0.1, 0.1, -0.1};
  EXPECT_DOUBLE_EQ(r.standard_error, BatchMeansStatistics(differences).standard_error);
  EXPECT_DOUBLE_EQ(r.estimate, 2.5);
  EXPECT_DOUBLE_EQ(r.reference, 2.55);
  EXPECT_TRUE(r.within_ceiling);
  EXPECT_FALSE(
      CheckPaired(kBatches, {reference.data(), 3}, {.b = 1, .se_ceiling = 1, .critical = 2})
          .passed());
}

}  // namespace
}  // namespace aadp
