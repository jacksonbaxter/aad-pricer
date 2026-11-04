// The h-study rule (bench/bump_study.hpp) on hand-made rows, and one live study at a small size.
#include "bump_study.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "aad_workloads.hpp"
#include "pricer/bump.hpp"

namespace aadp::bench::aad {
namespace {

// Rows of a central study: truncation c h^2 falling 4 times per halving, round-off r / h rising,
// so each change is stable once the truncation is below the round-off of its pair.
std::vector<StudyPoint> Points(double c, double r, std::size_t count) {
  std::vector<StudyPoint> points;
  for (std::size_t j = 0; j < count; ++j) {
    const double h = std::ldexp(1.0, -static_cast<int>(j));
    StudyPoint p;
    p.h = h;
    p.denominator = 2.0 * h;
    p.estimate.mean = 1.0 + (c * h * h);
    p.roundoff = r / h;
    points.push_back(p);
  }
  return points;
}

TEST(BumpStudy, ChoosesTheFirstOfTwoStablePairs) {
  // change_j = c h^2 (3/4); limit_j = r / h + 2 r / h = 3 r / h. Stable once 0.75 c h^3 <= 3 r.
  const std::vector<StudyRow> rows = Tabulate(Points(1.0, 1e-6, 20));
  const StudyChoice choice = ChooseStep(rows, pricer::BumpScheme::kCentral);
  ASSERT_TRUE(choice.chosen.has_value());
  // 0.75 h^3 <= 3e-6 first at h = 2^-6 (h^3 = 3.8e-6 at 2^-6 is too big; 2^-7 gives 4.8e-7).
  for (std::size_t j = 0; j < *choice.chosen; ++j)
    EXPECT_FALSE(rows[j].stable && rows[j + 1].stable);
  EXPECT_TRUE(rows[*choice.chosen].stable);
  EXPECT_TRUE(rows[*choice.chosen + 1].stable);
  EXPECT_DOUBLE_EQ(choice.bias, (4.0 / 3.0) * rows[*choice.chosen].change);
  EXPECT_FALSE(choice.violation.has_value());
}

TEST(BumpStudy, OneStablePairAloneIsNotEnough) {
  std::vector<StudyRow> rows = Tabulate(Points(1.0, 1e-6, 20));
  const std::size_t first = *ChooseStep(rows, pricer::BumpScheme::kCentral).chosen;
  // A truncation crossing zero makes one early row look stable: it must not be chosen.
  ASSERT_GE(first, 3U);
  rows[1].stable = true;
  EXPECT_EQ(ChooseStep(rows, pricer::BumpScheme::kCentral).chosen, first);
}

TEST(BumpStudy, ReportsARoundOffScaleThatDidNotHold) {
  std::vector<StudyRow> rows = Tabulate(Points(1.0, 1e-6, 20));
  const std::size_t chosen = *ChooseStep(rows, pricer::BumpScheme::kCentral).chosen;
  rows[chosen + 4].stable = false;
  const StudyChoice choice = ChooseStep(rows, pricer::BumpScheme::kCentral);
  EXPECT_EQ(choice.chosen, chosen);
  EXPECT_EQ(choice.violation, chosen + 4);
}

TEST(BumpStudy, ForwardBiasIsTwiceTheChange) {
  const std::vector<StudyRow> rows = Tabulate(Points(1.0, 1e-6, 20));
  const StudyChoice choice = ChooseStep(rows, pricer::BumpScheme::kForward);
  ASSERT_TRUE(choice.chosen.has_value());
  EXPECT_DOUBLE_EQ(choice.bias, 2.0 * rows[*choice.chosen].change);
}

TEST(BumpStudy, ALiveStudyStabilisesAndItsRoundOffScaleHolds) {
  const Workload* w = FindWorkload("aad4-asian12");
  ASSERT_NE(w, nullptr);
  auto config = ConfigFor(*w, 500, 1);
  ASSERT_TRUE(config.has_value());
  config->threads = 4;
  const TapeShape shape = MeasureTape(*w, *config);
  const double n = RoundingCount(static_cast<double>(shape.statements_per_path),
                                 config->paths / config->batches, config->batches);
  const pricer::BumpSteps h0 = InitialSteps(w->market);
  for (const pricer::BumpScheme scheme :
       {pricer::BumpScheme::kCentral, pricer::BumpScheme::kForward}) {
    for (std::size_t i = 0; i < pricer::kInputCount; ++i) {
      const auto points =
          PricerStudy(*w, *config, static_cast<pricer::Input>(i), scheme, h0[i], kStudyHalvings, n);
      const std::vector<StudyRow> rows = Tabulate(points);
      const StudyChoice choice = ChooseStep(rows, scheme);
      EXPECT_TRUE(choice.chosen.has_value()) << InputName(i) << " " << SchemeName(scheme);
      EXPECT_FALSE(choice.violation.has_value())
          << InputName(i) << " " << SchemeName(scheme) << ", row " << choice.violation.value_or(0);
    }
  }
}

TEST(BumpStudy, TheLaxWendroffStudyFindsNoTruncation) {
  const Workload* w = FindWorkload("lw-worst");
  ASSERT_NE(w, nullptr);
  const TapeShape shape = MeasureTape(*w, {});
  const double n = RoundingCount(static_cast<double>(shape.statements_per_path), 0, 0);
  // J is linear, so every step is stable from the first: the central and forward quotients are
  // the gradient up to round-off at any h.
  for (const pricer::BumpScheme scheme :
       {pricer::BumpScheme::kCentral, pricer::BumpScheme::kForward}) {
    const std::vector<StudyRow> rows = LwStudy(scheme, kLwInitialStep, 8, n);
    const StudyChoice choice = ChooseStep(rows, scheme);
    EXPECT_EQ(choice.chosen, 0U) << SchemeName(scheme);
    EXPECT_FALSE(choice.violation.has_value()) << SchemeName(scheme);
  }
}

}  // namespace
}  // namespace aadp::bench::aad
