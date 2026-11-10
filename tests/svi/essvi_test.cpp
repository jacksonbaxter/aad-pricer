// eSSVI slices, their raw SVI conversion, Mingone's global map, the constraint checker and the
// clamps (src/svi/essvi.hpp), on hand-made parameters only. Every slice, seed and tolerance here
// was stated in NOTES.md, "Stage 7c", before any of these tests ran.
#include "svi/essvi.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "ad/dual.hpp"
#include "ad/real.hpp"  // IWYU pragma: keep (the operations essvi.hpp applies to ad::Real)
#include "ad/tape.hpp"
#include "rng/philox.hpp"
#include "svi/arbitrage.hpp"
#include "svi/raw_svi.hpp"

namespace aadp::svi {
namespace {

using Slice = EssviSlice<double>;
using Surface = std::vector<Slice>;

constexpr double kEps = 0x1p-52;
// A development seed, never 1729 or 271828.
constexpr std::uint64_t kBoxSeed = 20261124;

// The hand-made surface H of the pre-registration.
const Surface kH = {{.theta = 4.4e-4, .rho = -0.5, .psi = 0.02},
                    {.theta = 1.0e-3, .rho = -0.45, .psi = 0.03},
                    {.theta = 2.8e-3, .rho = -0.5, .psi = 0.05}};

Surface WithPsi(Surface s, std::size_t j, double psi) {
  s[j].psi = psi;
  return s;
}

// H_B: H with psi_2 = 0.04, so that psi_1 can reach its cap without breaking N.
const Surface kHB = WithPsi(kH, 1, 0.04);

double P(const Slice& lo, const Slice& hi) {
  return std::max((1.0 + lo.rho) / (1.0 + hi.rho), (1.0 - lo.rho) / (1.0 - hi.rho));
}

std::vector<double> Coordinates(const Surface& s) {
  const EssviGlobalPoint g = ToGlobal(s);
  EXPECT_TRUE(g.mapped);
  EXPECT_TRUE(g.clamps.empty());
  return g.x;
}

std::vector<PsiInterval> Intervals(std::span<const double> x) {
  std::vector<PsiInterval> out;
  (void)FromGlobal<double>(x, &out);
  return out;
}

std::vector<EssviConstraint> Violated(const EssviConstraintReport& r) {
  std::vector<EssviConstraint> out;
  for (const EssviConstraintState& s : r.states) {
    if (s.violated) out.push_back(s.constraint);
  }
  return out;
}

const EssviConstraintState* Find(const EssviConstraintReport& r, EssviConstraint c,
                                 std::size_t slice) {
  for (const EssviConstraintState& s : r.states) {
    if (s.constraint == c && s.slice == slice) return &s;
  }
  return nullptr;
}

TEST(Essvi, RawConversionGivesTheSameTotalVariance) {
  const double rho_end = kRhoBound;
  const double theta_end = 0.01;
  const std::vector<Slice> slices = {
      kH[0],
      kH[1],
      kH[2],
      // Corbetta et al., Table 1, at 0.030137 and 2.945205 years.
      {.theta = 0.0001, .rho = -0.224, .psi = 0.012},
      {.theta = 0.0750, .rho = -0.724, .psi = 0.243},
      // Each end of the rho box, psi at B2.
      {.theta = theta_end, .rho = -rho_end, .psi = std::sqrt(4.0 * theta_end / (1.0 + rho_end))},
      {.theta = theta_end, .rho = rho_end, .psi = std::sqrt(4.0 * theta_end / (1.0 + rho_end))}};
  for (const Slice& s : slices) {
    const SviParams raw = ToRawSvi(s);
    double worst = 0.0;
    for (int i = -300; i <= 300; ++i) {
      const double k = 0.01 * i;
      const double u = (s.psi * k) + (s.theta * s.rho);
      const double root = std::sqrt((u * u) + (s.theta * s.theta * (1.0 - (s.rho * s.rho))));
      const double scale = s.theta + (s.psi * std::fabs(k)) + root;
      const double diff = std::fabs(TotalVariance(raw, k) - EssviTotalVariance(s, k));
      worst = std::max(worst, diff / (64.0 * kEps * scale));
    }
    EXPECT_LE(worst, 1.0) << "theta " << s.theta << " rho " << s.rho << " psi " << s.psi;
    EXPECT_NEAR(EssviTotalVariance(s, 0.0), s.theta, 4.0 * kEps * s.theta);
  }
}

TEST(Essvi, HandMadeSurfaceIsInsideEveryConstraintWithSlackAtLeast034) {
  const EssviConstraintReport r = CheckEssviConstraints(kH);
  EXPECT_EQ(r.violations, 0U);
  ASSERT_EQ(r.states.size(), 12U);
  for (const EssviConstraintState& s : r.states) {
    // 0.34 exactly for S on slice 2 (psi ratio 1.5 against theta ratio 1e-3 / 4.4e-4), so to
    // rounding.
    EXPECT_GE(s.slack, 0.34 - (64.0 * kEps))
        << EssviConstraintName(s.constraint) << " slice " << s.slice;
    EXPECT_FALSE(s.active);
  }
  const std::vector<PsiInterval> iv = Intervals(Coordinates(kH));
  EXPECT_EQ(iv[0].cap, PsiCap::kGatheralJacquier);
  EXPECT_EQ(iv[1].cap, PsiCap::kThetaRatio);
  EXPECT_EQ(iv[2].cap, PsiCap::kThetaRatio);
}

TEST(Essvi, EveryBoxPointSatisfiesTheConstraintsAndStage6aChecks) {
  constexpr std::size_t kPoints = 300;
  constexpr std::size_t kSlices = 3;
  std::size_t inside_domain = 0;
  for (std::uint64_t s = 0; s < kPoints; ++s) {
    const auto u = [&](std::size_t c) {
      return PathUniform(kBoxSeed, s, static_cast<std::uint32_t>(c));
    };
    std::vector<double> x(3 * kSlices);
    for (std::size_t i = 0; i < kSlices; ++i) x[i] = -kRhoBound + (2.0 * kRhoBound * u(i));
    x[kSlices] = std::pow(10.0, -4.0 + (3.0 * u(kSlices)));
    for (std::size_t i = 1; i < kSlices; ++i) x[kSlices + i] = 2.0 * x[kSlices] * u(kSlices + i);
    for (std::size_t i = 0; i < kSlices; ++i) {
      const double v = u((2 * kSlices) + i);
      const double lo = i == 0 ? kC1Lower : 0.0;
      double c = lo + ((1.0 - lo) * u(10 + i));
      if (v < 0.25) {
        c = 1.0;
      } else if (v < 0.5) {
        c = lo;
      }
      x[(2 * kSlices) + i] = c;
    }
    const Surface slices = FromGlobal<double>(x);
    const EssviConstraintReport r = CheckEssviConstraints(slices);
    EXPECT_EQ(r.violations, 0U) << "point " << s;
    std::vector<SviParams> raw;
    bool inside = true;
    for (const Slice& sl : slices) {
      raw.push_back(ToRawSvi(sl));
      inside = inside && CheckParameters(raw.back()).empty();
    }
    if (!inside) continue;
    ++inside_domain;
    for (std::size_t j = 0; j < kSlices; ++j) {
      const ButterflyReport b = CheckButterfly(raw[j]);
      EXPECT_TRUE(b.violations.empty())
          << "point " << s << " slice " << j << ": " << KindName(b.violations.front().kind);
    }
    for (std::size_t j = 1; j < kSlices; ++j) {
      const CalendarReport c = CheckCalendar(raw[j - 1], raw[j]);
      EXPECT_TRUE(c.violations.empty())
          << "point " << s << " pair " << j << ": " << KindName(c.violations.front().kind);
    }
  }
  EXPECT_GE(inside_domain, 290U);
  std::printf("box points inside stage 6a's domain: %zu of %zu\n", inside_domain, kPoints);
}

// One case of the just-outside test: the surface, the constraint it breaks, the coordinate whose
// clamp stands for it, and the end that clamp must reach.
struct Outside {
  const char* name;
  Surface surface;
  EssviConstraint constraint;
  std::size_t constraint_slice;
  EssviCoordinate coordinate;
  std::size_t coordinate_slice;
  double end;
};

// psi_1's cap on H_B, as the map has it.
double CapOfPsi1() { return Intervals(Coordinates(kHB))[0].upper; }

std::vector<Outside> OutsideCases(double sign) {
  const double e = sign * 1e-6;
  const double p2 = P(kH[0], kH[1]);
  Surface a_case = kH;
  a_case[1].theta = kH[0].theta * p2 * (1.0 - e);
  a_case[1].psi = kH[0].psi * p2 * (sign > 0.0 ? 1.0 : 1.0 + 5e-7);
  return {
      {.name = "B2",
       .surface = WithPsi(kHB, 0, CapOfPsi1() * (1.0 + e)),
       .constraint = EssviConstraint::kGatheralJacquier,
       .constraint_slice = 0,
       .coordinate = EssviCoordinate::kC,
       .coordinate_slice = 0,
       .end = 1.0},
      // On H, psi_2 at its S cap puts psi_3 = 0.05 exactly at N's lower bound 1.1 psi_2, so the
      // case raises psi_3 to 0.06, as the fit test's surface outside S does.
      {.name = "S",
       .surface =
           WithPsi(WithPsi(kH, 2, 0.06), 1, kH[0].psi * kH[1].theta / kH[0].theta * (1.0 + e)),
       .constraint = EssviConstraint::kThetaRatio,
       .constraint_slice = 1,
       .coordinate = EssviCoordinate::kC,
       .coordinate_slice = 1,
       .end = 1.0},
      {.name = "N-psi",
       .surface = WithPsi(kH, 1, kH[0].psi * p2 * (1.0 - e)),
       .constraint = EssviConstraint::kPsiLower,
       .constraint_slice = 1,
       .coordinate = EssviCoordinate::kC,
       .coordinate_slice = 1,
       .end = 0.0},
      {.name = "a < 0",
       .surface = a_case,
       .constraint = EssviConstraint::kThetaRatio,
       .constraint_slice = 1,
       .coordinate = EssviCoordinate::kA,
       .coordinate_slice = 1,
       .end = 0.0},
  };
}

TEST(Essvi, JustOutsideAConstraintIsRejectedAndClampedWithTheClampRecorded) {
  for (const Outside& o : OutsideCases(+1.0)) {
    SCOPED_TRACE(o.name);
    const EssviConstraintReport r = CheckEssviConstraints(o.surface);
    ASSERT_EQ(Violated(r), std::vector<EssviConstraint>{o.constraint});
    const EssviConstraintState* st = Find(r, o.constraint, o.constraint_slice);
    ASSERT_NE(st, nullptr);
    EXPECT_TRUE(st->violated);
    const EssviGlobalPoint g = ToGlobal(o.surface);
    ASSERT_TRUE(g.mapped);
    ASSERT_EQ(g.clamps.size(), 1U);
    const EssviClamp& c = g.clamps.front();
    EXPECT_EQ(c.coordinate, o.coordinate);
    EXPECT_EQ(c.slice, o.coordinate_slice);
    EXPECT_EQ(c.clamped, o.end);
    EXPECT_NE(c.value, o.end);
    EXPECT_EQ(o.end == 0.0 ? c.value < 0.0 : c.value > 1.0, true) << c.value;
    const Surface clamped = FromGlobal<double>(g.x);
    const EssviConstraintReport after = CheckEssviConstraints(clamped);
    EXPECT_EQ(after.violations, 0U);
    const EssviConstraintState* now = Find(after, o.constraint, o.constraint_slice);
    ASSERT_NE(now, nullptr);
    EXPECT_TRUE(now->active) << now->slack;
  }
}

TEST(Essvi, RhoBeyondTheBoxIsClampedAndRecordedThoughNoConstraintBreaks) {
  // One slice: after H's slices, N would need psi_3 >= psi_2 (1 - rho_2) / (1 - rho_3), beyond B1.
  const Surface s = {{.theta = 0.1, .rho = 1.0 - 1e-7, .psi = 0.001}};
  EXPECT_EQ(CheckEssviConstraints(s).violations, 0U);
  const EssviGlobalPoint g = ToGlobal(s);
  ASSERT_TRUE(g.mapped);
  ASSERT_FALSE(g.clamps.empty());
  EXPECT_EQ(g.clamps.front().coordinate, EssviCoordinate::kRho);
  EXPECT_EQ(g.clamps.front().slice, 0U);
  EXPECT_EQ(g.clamps.front().value, 1.0 - 1e-7);
  EXPECT_EQ(g.clamps.front().clamped, kRhoBound);
  EXPECT_EQ(CheckEssviConstraints(FromGlobal<double>(g.x)).violations, 0U);
}

TEST(Essvi, JustInsideIsNeitherRejectedNorClampedAndMapsBackToItself) {
  for (const Outside& o : OutsideCases(-1.0)) {
    SCOPED_TRACE(o.name);
    EXPECT_EQ(CheckEssviConstraints(o.surface).violations, 0U);
    const EssviGlobalPoint g = ToGlobal(o.surface);
    ASSERT_TRUE(g.mapped);
    EXPECT_TRUE(g.clamps.empty());
    const Surface back = FromGlobal<double>(g.x);
    for (std::size_t j = 0; j < back.size(); ++j) {
      EXPECT_NEAR(back[j].theta, o.surface[j].theta, 1e-12 * o.surface[j].theta) << j;
      EXPECT_NEAR(back[j].rho, o.surface[j].rho, 1e-12 * std::fabs(o.surface[j].rho)) << j;
      EXPECT_NEAR(back[j].psi, o.surface[j].psi, 1e-12 * o.surface[j].psi) << j;
    }
  }
}

TEST(Essvi, CheckerHoldsB1StrictAndB2AtEqualityActive) {
  const double theta = 2.0;
  const double rho = 0.25;
  // psi (1 + |rho|) = 4 exactly: B1, strict, broken. B2 holds: psi^2 (1 + |rho|) = 12.8 <= 32.
  const Slice lee{.theta = 8.0, .rho = rho, .psi = 4.0 / (1.0 + rho)};
  const EssviConstraintReport r1 = CheckEssviConstraints(std::vector<Slice>{lee});
  EXPECT_EQ(Violated(r1), std::vector<EssviConstraint>{EssviConstraint::kLee});
  const Slice gj{.theta = theta, .rho = rho, .psi = std::sqrt(4.0 * theta / (1.0 + rho))};
  const EssviConstraintReport r2 = CheckEssviConstraints(std::vector<Slice>{gj});
  EXPECT_EQ(r2.violations, 0U);
  const EssviConstraintState* st = Find(r2, EssviConstraint::kGatheralJacquier, 0);
  ASSERT_NE(st, nullptr);
  EXPECT_TRUE(st->active);
  // On the map: one slice whose theta lets B1 cap psi (sqrt(4 theta / 1.25) > 3.2), c_1 = 1. The
  // margin keeps B1 strict: its slack is the margin, to rounding.
  std::vector<PsiInterval> iv;
  const Surface capped = FromGlobal<double>(std::vector<double>{rho, 10.0, 1.0}, &iv);
  ASSERT_EQ(iv.size(), 1U);
  EXPECT_EQ(iv[0].cap, PsiCap::kLee);
  const EssviConstraintReport r3 = CheckEssviConstraints(capped);
  EXPECT_EQ(r3.violations, 0U);
  const EssviConstraintState* at_lee = Find(r3, EssviConstraint::kLee, 0);
  ASSERT_NE(at_lee, nullptr);
  EXPECT_NEAR(at_lee->slack, kLeeMargin, 64.0 * kEps);
  const Slice bad{.theta = theta, .rho = 1.0, .psi = 0.1};
  EXPECT_EQ(Violated(CheckEssviConstraints(std::vector<Slice>{bad})),
            std::vector<EssviConstraint>{EssviConstraint::kParameters});
}

TEST(Essvi, OnTheTapeValuesAreTheDoubleBitsAndTheGradientIsTheDuals) {
  const std::vector<double> x = Coordinates(kH);
  for (const double k : {-0.2, -0.05, 0.0, 0.03, 0.1}) {
    for (std::size_t j = 0; j < kH.size(); ++j) {
      ad::Tape tape;
      const ad::TapeScope scope(tape);
      std::vector<ad::Real> xr;
      xr.reserve(x.size());
      for (const double v : x) xr.push_back(tape.Register(v));
      const std::vector<EssviSlice<ad::Real>> s = FromGlobal<ad::Real>(xr);
      const std::vector<Slice> d = FromGlobal<double>(x);
      EXPECT_EQ(s[j].theta.value(), d[j].theta);
      EXPECT_EQ(s[j].rho.value(), d[j].rho);
      EXPECT_EQ(s[j].psi.value(), d[j].psi);
      const RawSvi<ad::Real> raw = ToRawSvi(s[j]);
      const SviParams raw_d = ToRawSvi(d[j]);
      EXPECT_EQ(raw.a.value(), raw_d.a);
      EXPECT_EQ(raw.b.value(), raw_d.b);
      EXPECT_EQ(raw.m.value(), raw_d.m);
      EXPECT_EQ(raw.sigma.value(), raw_d.sigma);
      const ad::Real w = EssviTotalVariance(s[j], ad::Real(k));
      EXPECT_EQ(w.value(), EssviTotalVariance(d[j], k));
      tape.AddAdjoint(w, 1.0);
      tape.SweepBack(tape.GetPosition(), ad::Position{});
      std::vector<double> dual(x.size());
      for (std::size_t c = 0; c < x.size(); ++c) {
        using D = ad::Dual<double, 1>;
        std::vector<D> xd(x.begin(), x.end());
        xd[c].tangent[0] = 1.0;
        const std::vector<EssviSlice<D>> sd = FromGlobal<D>(std::span<const D>(xd));
        dual[c] = EssviTotalVariance(sd[j], D(k)).tangent[0];
      }
      double scale = 0.0;
      for (const double g : dual) scale = std::max(scale, std::fabs(g));
      ASSERT_GT(scale, 0.0);
      for (std::size_t c = 0; c < x.size(); ++c) {
        EXPECT_LE(std::fabs(tape.Adjoint(xr[c]) - dual[c]), 0x1p-40 * scale)
            << "slice " << j << " coordinate " << c << " k " << k;
      }
    }
  }
}

}  // namespace
}  // namespace aadp::svi
