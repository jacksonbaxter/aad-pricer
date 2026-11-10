// Check 1: the pricer against the frozen reference kernels in src/mc/, path by path, on every
// product both price, with the same Philox draws.
//
// The two are not bit-identical: the reference calls libm's exp, log and erfc, the pricer its own.
// The bound, stated before the comparison ran, is per path and per value: the error tracker
// (error_tracker.hpp) runs the pricer's kernel on our functions and on libm's side by side, and
// carries forward a bound on their difference from
//   - stage 1a's measured errors of our exp (0.613 ULP) and log (0.840 ULP) against exact values,
//     and the premise that libm's are below 1 ULP, checked here on stage 1a's table, so each
//     function call adds at most 1.613 or 1.840 ULP between the two sides;
//   - AS241 within 1.0e-15 relative on each side (stage 1a target; measured 8.39e-16 for ours,
//     8.47e-16 for the reference in Phase 2), except that in the central region the same input
//     gives the same bits, as stage 1a showed;
//   - erfc within 4 ULP on each side (normal_cdf_test.cpp measures both);
//   - one rounding of each side per arithmetic operation.
// Why that is the right bound: the kernels do the same operations in the same order, so those
// are the only places the two can part, and first-order propagation through the path's own
// values says how far each parting travels. Three claims are checked per path:
//   1. the tracker's own value is the double pricer's value, bit for bit, so the bound is about
//      the code that ships;
//   2. its libm twin is the reference kernel's value, bit for bit, so the operations really are
//      the same and the two can part only where the bound says;
//   3. |pricer - reference| <= bound, for the discounted payoff and for the survival weight.
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dual_ops.hpp"  // IWYU pragma: keep (the Dual overloads, found by ADL)
#include "error_tracker.hpp"
#include "math/exp.hpp"
#include "math/log.hpp"
#include "math/reference_table.hpp"
#include "mc/gbm_kernels.hpp"
#include "oracle/black_scholes.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/from_plan.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "rng/normal.hpp"
#include "rng/philox.hpp"
#include "support/dual.hpp"
#include "support/mc_support.hpp"

#ifndef AADP_SMOKE_SIZE
#error "cmake/area_pricer.cmake sets AADP_SMOKE_SIZE"
#endif

namespace aadp::pricer {
namespace {

using testing::Accuracy;
using testing::Tracked;

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261106;
constexpr std::uint64_t kPaths = AADP_SMOKE_SIZE ? 1024 : 16384;
// A path whose bound is this large a fraction of its scale had a branch the bound could not
// settle. Counted and reported; the claim does not exclude them, their bound just covers both.
constexpr double kUnsureFraction = 1e-6;

struct Case {
  std::string name;
  aadp::Contract contract;
  aadp::MarketParams<double> market;
};

aadp::Contract RefContract(aadp::Product product, aadp::OptionType type, double strike,
                           double barrier, std::uint32_t steps, bool include_spot) {
  return {.product = product,
          .type = type,
          .strike = strike,
          .barrier = barrier,
          .steps = steps,
          .include_spot = include_spot};
}

// Parameters of the GBM rows in references/comparison_plan.csv, plus puts and a K < H bridge.
// The 57 rows of stage 8 are compared by aadp_knock_in_rows_test and aadp_svi_rows_test.
std::vector<Case> Cases() {
  using aadp::OptionType;
  using aadp::Product;
  const aadp::MarketParams<double> va{
      .spot = 105.0, .sigma = 0.36, .r = 0.1, .q = 0.1, .maturity = 0.5};
  const aadp::MarketParams<double> vc{
      .spot = 430.0, .sigma = 0.2, .r = 0.07, .q = 0.05, .maturity = 1.0 / 12.0};
  const aadp::MarketParams<double> ga{
      .spot = 100.0, .sigma = 0.2, .r = 0.06, .q = 0.03, .maturity = 1.0};
  const aadp::MarketParams<double> aa{
      .spot = 100.0, .sigma = 0.17801, .r = 0.0367, .q = 0.0, .maturity = 1.0};
  const aadp::MarketParams<double> db{
      .spot = 100.0, .sigma = 0.3, .r = 0.1, .q = 0.0, .maturity = 0.2};
  const aadp::MarketParams<double> dbs{
      .spot = 100.0, .sigma = 0.6, .r = 0.1, .q = 0.0, .maturity = 0.2};
  const aadp::MarketParams<double> cbk{
      .spot = 100.0, .sigma = 0.25, .r = 0.08, .q = 0.04, .maturity = 0.5};
  return {
      {.name = "european call",
       .contract = RefContract(Product::kEuropean, OptionType::kCall, 100, 0, 1, false),
       .market = va},
      {.name = "european put",
       .contract = RefContract(Product::kEuropean, OptionType::kPut, 405, 0, 1, false),
       .market = vc},
      {.name = "european call, 12 steps",
       .contract = RefContract(Product::kEuropean, OptionType::kCall, 100, 0, 12, false),
       .market = va},
      {.name = "arithmetic Asian with spot",
       .contract = RefContract(Product::kAsianArithmetic, OptionType::kCall, 100, 0, 12, true),
       .market = aa},
      {.name = "arithmetic Asian with spot, 50",
       .contract = RefContract(Product::kAsianArithmetic, OptionType::kCall, 90, 0, 50, true),
       .market = aa},
      {.name = "arithmetic Asian without spot, put",
       .contract = RefContract(Product::kAsianArithmetic, OptionType::kPut, 100, 0, 12, false),
       .market = ga},
      {.name = "geometric Asian",
       .contract = RefContract(Product::kAsianGeometric, OptionType::kCall, 100, 0, 10, false),
       .market = ga},
      {.name = "geometric Asian with spot, put",
       .contract = RefContract(Product::kAsianGeometric, OptionType::kPut, 100, 0, 10, true),
       .market = ga},
      {.name = "discrete hard",
       .contract = RefContract(Product::kDownOutHard, OptionType::kCall, 100, 95, 50, false),
       .market = db},
      {.name = "discrete one-step",
       .contract = RefContract(Product::kDownOutOneStep, OptionType::kCall, 100, 95, 50, false),
       .market = db},
      {.name = "discrete one-step, H 99",
       .contract = RefContract(Product::kDownOutOneStep, OptionType::kCall, 100, 99, 50, false),
       .market = db},
      {.name = "discrete one-step, sigma 0.6",
       .contract = RefContract(Product::kDownOutOneStep, OptionType::kCall, 100, 95, 4, false),
       .market = dbs},
      {.name = "discrete one-step, put",
       .contract = RefContract(Product::kDownOutOneStep, OptionType::kPut, 100, 95, 50, false),
       .market = db},
      {.name = "continuous bridge",
       .contract = RefContract(Product::kDownOutBridge, OptionType::kCall, 100, 95, 50, false),
       .market = db},
      {.name = "continuous bridge, K below H",
       .contract = RefContract(Product::kDownOutBridge, OptionType::kCall, 90, 95, 25, false),
       .market = cbk},
  };
}

Market<Tracked> TrackedMarket(const aadp::MarketParams<double>& m) {
  return {.spot = m.spot, .sigma = m.sigma, .r = m.r, .q = m.q, .maturity = m.maturity};
}

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

struct Summary {
  double worst_ratio = 0.0;     // max |pricer - reference| / bound
  double worst_relative = 0.0;  // max |pricer - reference| / scale
  double worst_bound = 0.0;     // max bound / scale
  std::uint64_t differing = 0;  // paths whose payoff bits differ
  std::uint64_t unsure = 0;
};

// Scale of a path's value: the discount times the larger of the strike and S0, so a relative
// figure means something at a payoff of zero too.
double Scale(const Case& c) {
  return std::exp(-c.market.r * c.market.maturity) * std::max(c.contract.strike, c.market.spot);
}

void Add(Summary& s, double pricer, double reference, const Tracked& t, double scale) {
  const double diff = std::fabs(pricer - reference);
  if (diff > 0.0) s.worst_ratio = std::max(s.worst_ratio, diff / t.bound);
  s.worst_relative = std::max(s.worst_relative, diff / scale);
  s.worst_bound = std::max(s.worst_bound, t.bound / scale);
}

TEST(PathAgreement, PricerMatchesTheReferenceWithinTheBound) {
  for (const Case& c : Cases()) {
    const Contract contract = FromPlan(c.contract);
    const Market<double> market = FromPlan(c.market);
    ASSERT_EQ(Validate(contract, market), ContractError::kNone) << c.name;
    const GbmModel<double> model(market, contract.steps);
    const GbmModel<Tracked> tracked_model(TrackedMarket(c.market), contract.steps);
    const double scale = Scale(c);
    std::vector<double> normal(contract.steps);
    std::vector<double> reference_uniforms(contract.steps);
    const PathUniforms u{.normal = normal, .crossing = {}};
    Summary value;
    Summary weight;
    for (std::uint64_t path = 0; path < kPaths; ++path) {
      // Each side draws its own uniforms, the reference through src/rng/philox.hpp directly, so a
      // pricer that keys Philox differently cannot pass.
      FillPath(kSeed, path, normal, {});
      for (std::uint32_t step = 0; step < contract.steps; ++step) {
        reference_uniforms[step] = PathUniform(kSeed, path, step);
      }
      const PathValue<double> own = EvaluatePath<double>(contract, model, u);
      const auto reference =
          aadp::DiscountedPayoff<double>(c.contract, c.market, reference_uniforms);
      const PathValue<Tracked> t = EvaluatePath<Tracked>(contract, tracked_model, u);

      ASSERT_EQ(Bits(t.value.own), Bits(own.value)) << c.name << ", path " << path;
      ASSERT_EQ(Bits(t.weight.own), Bits(own.weight)) << c.name << ", path " << path;
      ASSERT_EQ(Bits(t.value.ref), Bits(reference)) << c.name << ", path " << path;
      ASSERT_LE(std::fabs(own.value - reference), t.value.bound) << c.name << ", path " << path;
      ASSERT_LE(std::fabs(own.weight - t.weight.ref), t.weight.bound)
          << c.name << ", path " << path;

      Add(value, own.value, reference, t.value, scale);
      Add(weight, own.weight, t.weight.ref, t.weight, 1.0);
      value.differing += Bits(own.value) != Bits(reference) ? 1U : 0U;
      value.unsure += t.value.unsure && t.value.bound > kUnsureFraction * scale ? 1U : 0U;
    }
    std::printf(
        "measured, %s, %llu paths: payoff bits differ on %llu; max |diff| / bound %.3g; max "
        "|diff| / scale %.3g; max bound / scale %.3g; weight max |diff| %.3g, bound %.3g; "
        "unsettled branches %llu%s\n",
        c.name.c_str(), static_cast<unsigned long long>(kPaths),
        static_cast<unsigned long long>(value.differing), value.worst_ratio, value.worst_relative,
        value.worst_bound, weight.worst_relative, weight.worst_bound,
        static_cast<unsigned long long>(value.unsure),
        AADP_SMOKE_SIZE ? " (reduced size, not a result)" : "");
    // A path only the bound's width separates from a kill would make this a weak check.
    EXPECT_EQ(value.unsure, 0U) << c.name;
  }
}

// The premises the bound takes from outside this test, measured on stage 1a's exact table.
TEST(PathAgreement, LibmPremisesHoldOnTheStage1aTable) {
  double exp_libm = 0.0;
  double exp_own = 0.0;
  for (const auto& p : math::testing::kExpReference) {
    if (std::isinf(p.hi) || p.hi == 0.0 || p.hi < 0x1p-1022) continue;
    exp_libm = std::max(exp_libm, math::testing::UlpError(std::exp(p.input), p.hi, p.lo));
    exp_own = std::max(exp_own, math::testing::UlpError(math::exp(p.input), p.hi, p.lo));
  }
  double log_libm = 0.0;
  double log_own = 0.0;
  for (const auto& p : math::testing::kLogReference) {
    if (std::isinf(p.hi) || std::isnan(p.hi) || p.hi == 0.0) continue;
    log_libm = std::max(log_libm, math::testing::UlpError(std::log(p.input), p.hi, p.lo));
    log_own = std::max(log_own, math::testing::UlpError(math::log(p.input), p.hi, p.lo));
  }
  double inverse_libm = 0.0;
  for (const auto& p : math::testing::kInverseNormalReference) {
    if (std::isinf(p.hi) || p.hi == 0.0) continue;
    inverse_libm = std::max(
        inverse_libm, math::testing::RelativeError(aadp::inverse_normal(p.input), p.hi, p.lo));
  }
  std::printf(
      "measured on stage 1a's table: libm exp %.4f ULP (ours %.4f), libm log %.4f ULP (ours "
      "%.4f), reference AS241 %.3g relative\n",
      exp_libm, exp_own, log_libm, log_own, inverse_libm);
  EXPECT_LT(exp_libm, Accuracy::kLibmExp);
  EXPECT_LT(log_libm, Accuracy::kLibmLog);
  EXPECT_LE(exp_own, Accuracy::kOwnExp);
  EXPECT_LE(log_own, Accuracy::kOwnLog);
  EXPECT_LE(inverse_libm, Accuracy::kInverseNormalRelative);
}

// The bound can fail: a pricer that drifts by a few ULP more than the accuracies allow is caught.
TEST(PathAgreement, BoundSeesAnErrorJustBeyondIt) {
  const Tracked x(4.605170185988091);
  const Tracked y = testing::Exp(x);
  const double off_by = y.bound * 1.5;
  EXPECT_GT(std::fabs((y.own + off_by) - y.ref), y.bound);
}

// The Dual cross-check: values are the double kernel's bits, and tangents agree with the
// reference kernel's Dual on the same paths. The tangents are the same chain rule evaluated at
// values that differ by the bound above, about 1e-14 of scale, and its amplification through a
// path is far below 1e3, so 1e-9 relative leaves a wide margin.
TEST(PathAgreement, DualMatchesTheDoubleKernelAndTheReferenceDual) {
  constexpr double kTangentRelative = 1e-9;
  for (const Case& c : Cases()) {
    const Contract contract = FromPlan(c.contract);
    const GbmModel<double> model(FromPlan(c.market), contract.steps);
    const Market<test::Dual> dual_market = FromPlan(test::ActiveMarket(c.market));
    const GbmModel<test::Dual> dual_model(dual_market, contract.steps);
    std::vector<double> normal(contract.steps);
    const PathUniforms u{.normal = normal, .crossing = {}};
    double worst = 0.0;
    for (std::uint64_t path = 0; path < kPaths / 8; ++path) {
      FillPath(kSeed, path, normal, {});
      const auto ours = DiscountedPayoff<test::Dual>(contract, dual_model, u);
      ASSERT_EQ(Bits(ours.value), Bits(DiscountedPayoff<double>(contract, model, u)));
      const auto theirs =
          aadp::DiscountedPayoff<test::Dual>(c.contract, test::ActiveMarket(c.market), normal);
      for (std::size_t i = 0; i < test::kTangents; ++i) {
        const double diff = std::fabs(ours.tangent[i] - theirs.tangent[i]);
        const double allowed = kTangentRelative * (std::fabs(theirs.tangent[i]) + 1.0);
        worst = std::max(worst, diff / allowed);
        ASSERT_LE(diff, allowed) << c.name << ", path " << path << ", tangent " << i;
      }
    }
    std::printf("measured, %s: max tangent |diff| / allowance %.3g\n", c.name.c_str(), worst);
  }
}

}  // namespace
}  // namespace aadp::pricer
