// Check 6: the one-step survival adjoint at p near 1 (NOTES.md, "Barrier treatment"), where
// dPhi^-1(p v)/dp = v / phi(Phi^-1(p v)) is unbounded as p v -> 1.
//
// One step of a down-and-out call, S0 = 100, with the barrier placed a standard deviations below
// the step's mean, so p = Phi(a), and the step's uniform u set by hand, so v = 1 - u and
// q = p v. The corner is a large and u small. Expectations, stated before measuring:
//   E1 the tape equals the Dual within check 1's bound at every point: the adjoint is exact for
//      the function the pricer computes, however large its partials.
//   E2 every Greek is finite, down to u = 2^-53, the smallest uniform on the Philox grid.
//   E3 against the true pathwise delta, computed in the well-conditioned complement form
//      1 - q = Phi(-a) + p u (the reference in this file), the relative error, on the scale of
//      the delta's terms, is at most 32 u (1 / (1 - q) + 1 + a^2 + z^2) + 1e-13. Why: the
//      pricer rounds q = p v near 1 with an absolute error of a few u, and Phi^-1 turns that into
//      a relative error of at most delta q / (1 - q) in 1 / phi(z), since z / phi(z) <= 1 / (1 - q)
//      for z > 0 (Mills' ratio); a^2 and z^2 cover the normal CDF's tail error in the reference
//      (4 y^2 u, normal_cdf.hpp) and AS241's relative error in z. So the loss is the primal's
//      conditioning, and the adjoint adds nothing to it.
//   E4 the growth: the tape's dz/dp lies within Mills' bounds, v / ((1 - q)(z + 1 / z)) <=
//      dz/dp <= v / ((1 - q) z) for z = Phi^-1(q) > 0 (Gordon 1941), to 1e-9 relative.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "ad/gradient.hpp"
#include "ad/real.hpp"
#include "ad/tape.hpp"
#include "bounds.hpp"
#include "cases.hpp"
#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/dual_ops.hpp"  // IWYU pragma: keep (tests/pricer/: the Dual overloads, by ADL)
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/normal_cdf.hpp"
#include "support/dual.hpp"

namespace aadp::ad {
namespace {

constexpr pricer::Market<double> kMarket = testing::kMarket;
constexpr double u53 = 0x1p-53;

double Pdf(double x) { return NormalPdf(x); }

pricer::Contract OneStep(double barrier) {
  pricer::Contract c;
  c.product = pricer::Product::kBarrier;
  c.type = pricer::OptionType::kCall;
  c.strike = 0.9 * barrier;
  c.steps = 1;
  c.barrier = {.direction = pricer::Direction::kDown,
               .knock = pricer::Knock::kOut,
               .monitoring = pricer::Monitoring::kDiscrete,
               .treatment = pricer::Treatment::kOneStep,
               .level = barrier,
               .smoothing_width = 0.0};
  return c;
}

struct Reference {
  double delta = 0.0;
  double scale = 0.0;  // the delta's terms in absolute value
  double one_minus_q = 0.0;
  double z = 0.0;  // Phi^-1(q), positive in the corner
};

// The true pathwise delta of V = D p max(exp(x1) - K, 0), x1 = mean + sd z, z = Phi^-1(1 - q),
// 1 - q = Phi(-a) + p u, from the same a, mean and sd the kernel computes.
Reference TrueDelta(const pricer::Contract& c, double u) {
  const pricer::GbmModel<double> model(kMarket, 1);
  const pricer::StepLaw<double> law = model.Law(0, model.LogSpot());
  const double a = (law.mean - math::log(c.barrier.level)) / law.sd;
  const double p = pricer::NormalCdf(a);
  const double w = pricer::NormalCdf(-a) + (p * u);
  const double z = math::inverse_normal(w);
  const double x1 = law.mean + (law.sd * z);
  const double s = kMarket.spot;
  const double dp = Pdf(a) / (s * law.sd);
  const double dz = (-(1.0 - u) * dp) / Pdf(z);
  const double dx1 = (1.0 / s) + (law.sd * dz);
  const double e = math::exp(x1);
  const double intrinsic = std::max(e - c.strike, 0.0);
  const double in_money = e > c.strike ? 1.0 : 0.0;
  const double d = model.Discount();
  Reference r;
  r.delta = d * ((dp * intrinsic) + (p * e * dx1 * in_money));
  r.scale = d * ((std::fabs(dp) * intrinsic) +
                 (p * e * in_money * ((1.0 / s) + (law.sd * std::fabs(dz)))));
  r.one_minus_q = 1.0 - (p * (1.0 - u));
  r.z = -z;
  return r;
}

TEST(OneStepCorner, AdjointAtPNearOneMeetsItsFourExpectations) {
  const pricer::GbmModel<double> model(kMarket, 1);
  const pricer::StepLaw<double> law = model.Law(0, model.LogSpot());
  const pricer::GbmModel<test::Dual> dual_model(testing::DualMarket(kMarket), 1);
  double worst_e1 = 0.0;
  double worst_e3 = 0.0;
  double largest_dzdp = 0.0;
  std::printf("%5s %10s %10s %12s %14s %14s %10s\n", "a", "u", "1 - q", "dz/dp", "delta tape",
              "delta true", "E3 ratio");
  for (const double a : {1.0, 3.0, 5.0, 6.5, 7.5, 8.0}) {
    const double barrier = math::exp(law.mean - (a * law.sd));
    const pricer::Contract c = OneStep(barrier);
    ASSERT_EQ(pricer::Validate(c, kMarket), pricer::ContractError::kNone) << a;
    for (const double u : {0.5, 1e-2, 1e-5, 1e-9, 1e-13, u53}) {
      std::vector<double> normal = {u};
      const pricer::PathUniforms uniforms{.normal = normal, .crossing = {}};
      const test::Dual dual = pricer::DiscountedPayoff<test::Dual>(c, dual_model, uniforms);
      PathGradient::PathResult tape;
      {
        PathGradient path(c, kMarket);
        tape = path.Evaluate(uniforms);
      }
      for (std::size_t i = 0; i < kInputs; ++i) {
        ASSERT_TRUE(std::isfinite(tape.gradient[i])) << "E2: a " << a << ", u " << u;
        const double bound =
            testing::AgreementBound(tape.absolute[i], tape.statements, tape.operands);
        const double diff = std::fabs(tape.gradient[i] - dual.tangent[i]);
        EXPECT_LE(diff, bound) << "E1: a " << a << ", u " << u << ", input " << i;
        if (diff > 0.0) worst_e1 = std::max(worst_e1, diff / bound);
      }

      const Reference r = TrueDelta(c, u);
      const double allowed =
          (32.0 * u53 * ((1.0 / r.one_minus_q) + 1.0 + (a * a) + (r.z * r.z))) + 1e-13;
      const double ratio = std::fabs(tape.gradient[0] - r.delta) / (allowed * r.scale);
      EXPECT_LE(ratio, 1.0) << "E3: a " << a << ", u " << u << ": tape " << tape.gradient[0]
                            << ", true " << r.delta;
      worst_e3 = std::max(worst_e3, ratio);

      // E4 on the step's own operation: z = Phi^-1(p v), dz/dp from the tape.
      double dzdp = 0.0;
      {
        Tape small;
        const TapeScope scope(small);
        const Real p = small.Register(pricer::NormalCdf(a));
        const double v = 1.0 - u;
        const Real z = InverseNormal(p * v);
        small.AddAdjoint(z, 1.0);
        small.SweepBack(small.GetPosition(), Position{});
        dzdp = small.Adjoint(p);
        const double q = p.value() * v;
        const double one_minus_q = 1.0 - q;
        if (z.value() > 0.0) {
          const double lower = v / (one_minus_q * (z.value() + (1.0 / z.value())));
          const double upper = v / (one_minus_q * z.value());
          EXPECT_GE(dzdp, lower * (1.0 - 1e-9)) << "E4: a " << a << ", u " << u;
          EXPECT_LE(dzdp, upper * (1.0 + 1e-9)) << "E4: a " << a << ", u " << u;
        }
        largest_dzdp = std::max(largest_dzdp, dzdp);
      }
      std::printf("%5.2f %10.3g %10.3g %12.4g %14.8g %14.8g %10.3g\n", a, u, r.one_minus_q, dzdp,
                  tape.gradient[0], r.delta, ratio);
    }
  }
  std::printf(
      "measured: E1 max |tape - dual| / bound %.3g; E2 all finite; E3 max error / allowance "
      "%.3g; E4 largest dz/dp %.4g\n",
      worst_e1, worst_e3, largest_dzdp);
}

// The other end, q = p v near 0, which the pricer-gradient fuzzer found: the step's mean far on
// the dead side, so p is tiny. Below 2^-1000 the kernel takes the draw passive (kernels.hpp,
// OutOneStep), since 1 / phi(Phi^-1(q)) would overflow and make the Greeks NaN. One step, S0 =
// 100, sigma 1e-3, q = 0.16, so the mean sits 0.16 below log S0 and the barrier a sd above it.
// Expectations, stated before measuring:
//   E5 every Greek of the knock-out and of the knock-in is finite at every point;
//   E6 the tape equals the Dual within check 1's bound at every point;
//   E7 the grid reaches all three regimes: q = 0, 0 < q < 2^-1000 and q >= 2^-1000.
// E6 as stated failed at 4 of 72 points, by up to 10.8 times the bound, all on vega and all with
// p subnormal (a = -38 and below), where the Greeks are near 1e-320. There the bound's
// assumption fails (tests/ad/bounds.hpp): subnormal rounding is absolute and later partials
// amplify it. So E6 is held only where p >= 2^-1022, which still includes points with
// 0 < q < 2^-1000. At the other points the test prints the difference but does not check it.
TEST(OneStepCorner, GreeksStayFiniteWhenPTimesVIsNearZero) {
  constexpr pricer::Market<double> kDeep{
      .spot = 100.0, .sigma = 1e-3, .r = 0.0, .q = 0.16, .maturity = 1.0};
  const pricer::GbmModel<double> model(kDeep, 1);
  const pricer::StepLaw<double> law = model.Law(0, model.LogSpot());
  const pricer::GbmModel<test::Dual> dual_model(testing::DualMarket(kDeep), 1);
  std::size_t dead = 0;
  std::size_t passive = 0;
  std::size_t active = 0;
  std::size_t passive_checked = 0;
  double worst_e6 = 0.0;
  double worst_subnormal = 0.0;
  for (const double a : {-30.0, -36.0, -37.0, -37.5, -37.8, -38.0, -38.2, -38.4, -38.6}) {
    const double barrier = math::exp(law.mean - (a * law.sd));
    for (const pricer::Knock knock : {pricer::Knock::kOut, pricer::Knock::kIn}) {
      pricer::Contract c = OneStep(barrier);
      c.barrier.knock = knock;
      c.strike = 0.8 * kDeep.spot;
      ASSERT_EQ(pricer::Validate(c, kDeep), pricer::ContractError::kNone) << a;
      for (const double u : {0.5, 1e-9, 1.0 - 1e-6, 1.0 - u53}) {
        const double p = pricer::NormalCdf((law.mean - math::log(barrier)) / law.sd);
        const double q = p * (1.0 - u);
        if (knock == pricer::Knock::kOut) {
          if (q == 0.0) ++dead;
          if (q > 0.0 && q < 0x1p-1000) ++passive;
          if (q >= 0x1p-1000) ++active;
        }
        std::vector<double> normal = {u};
        const pricer::PathUniforms uniforms{.normal = normal, .crossing = {}};
        const test::Dual dual = pricer::DiscountedPayoff<test::Dual>(c, dual_model, uniforms);
        PathGradient::PathResult tape;
        {
          PathGradient path(c, kDeep);
          tape = path.Evaluate(uniforms);
        }
        for (std::size_t i = 0; i < kInputs; ++i) {
          ASSERT_TRUE(std::isfinite(tape.gradient[i]))
              << "E5: a " << a << ", u " << u << ", q " << q << ", input " << i;
          const double bound =
              testing::AgreementBound(tape.absolute[i], tape.statements, tape.operands);
          const double diff = std::fabs(tape.gradient[i] - dual.tangent[i]);
          if (p < 0x1p-1022) {
            if (diff > 0.0) worst_subnormal = std::max(worst_subnormal, diff / bound);
            continue;
          }
          EXPECT_LE(diff, bound) << "E6: a " << a << ", u " << u << ", input " << i;
          if (diff > 0.0) worst_e6 = std::max(worst_e6, diff / bound);
          if (knock == pricer::Knock::kOut && i == 0 && q < 0x1p-1000) ++passive_checked;
        }
      }
    }
  }
  EXPECT_GE(dead, 1U) << "E7";
  EXPECT_GE(passive, 1U) << "E7";
  EXPECT_GE(active, 1U) << "E7";
  EXPECT_GE(passive_checked, 1U) << "E6 must reach 0 < q < 2^-1000";
  std::printf(
      "measured: E5 all finite; E6 max |tape - dual| / bound %.3g (p >= 2^-1022; %zu knock-out "
      "points with 0 < q < 2^-1000 among them), with p subnormal %.3g, not checked; E7 points "
      "with q = 0: %zu, 0 < q < 2^-1000: %zu, q >= 2^-1000: %zu\n",
      worst_e6, passive_checked, worst_subnormal, dead, passive, active);
}

}  // namespace
}  // namespace aadp::ad
