// Check 1: per path, on every pricer product and treatment, the tape's gradient against the
// forward-mode Dual of tests/support/ on the same kernel and the same uniforms.
//
// Bound, stated before the comparison ran: bounds.hpp, |tape - dual| <= 2 gamma_n A (1 + 2
// gamma_n) + n 2^-1070 per input, with A the path's absolute gradient and n = 2 T + O from the
// path's own tape. The reason: both compute the same pathwise derivative from the same partials,
// in different association, and that bounds what association can change. Also checked: the
// tape's value is the double kernel's, bit for bit, as is the Dual's.
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ad/gradient.hpp"
#include "bounds.hpp"
#include "cases.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/dual_ops.hpp"  // IWYU pragma: keep (tests/pricer/: the Dual overloads, by ADL)
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "support/dual.hpp"

namespace aadp::ad {
namespace {

using testing::AllCases;
using testing::Case;
using testing::kMarket;

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261110;
constexpr std::uint64_t kPaths = 512;

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

struct Worst {
  double ratio = 0.0;     // max |tape - dual| / bound
  double relative = 0.0;  // max |tape - dual| / A
  std::size_t statements = 0;
};

void CompareCase(const Case& c, Worst& worst) {
  const pricer::GbmModel<double> model(kMarket, c.contract.steps);
  const pricer::GbmModel<test::Dual> dual_model(testing::DualMarket(kMarket), c.contract.steps);
  PathGradient tape(c.contract, kMarket);
  std::vector<double> normal(c.contract.steps);
  std::vector<double> crossing(c.contract.steps);
  const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
  for (std::uint64_t path = 0; path < kPaths; ++path) {
    pricer::FillPath(kSeed, path, normal, crossing);
    const double value = pricer::DiscountedPayoff<double>(c.contract, model, u);
    const test::Dual dual = pricer::DiscountedPayoff<test::Dual>(c.contract, dual_model, u);
    const PathGradient::PathResult result = tape.Evaluate(u);
    EXPECT_EQ(Bits(result.value), Bits(value)) << c.name << ", path " << path;
    EXPECT_EQ(Bits(dual.value), Bits(value)) << c.name << ", path " << path;
    worst.statements = std::max(worst.statements, result.statements);
    for (std::size_t i = 0; i < kInputs; ++i) {
      const double bound =
          testing::AgreementBound(result.absolute[i], result.statements, result.operands);
      const double diff = std::fabs(result.gradient[i] - dual.tangent[i]);
      ASSERT_TRUE(std::isfinite(result.gradient[i])) << c.name << ", path " << path;
      ASSERT_LE(diff, bound) << c.name << ", path " << path << ", input " << i << ": tape "
                             << result.gradient[i] << ", dual " << dual.tangent[i];
      if (diff > 0.0) {
        worst.ratio = std::max(worst.ratio, diff / bound);
        worst.relative = std::max(worst.relative, diff / result.absolute[i]);
      }
    }
  }
}

TEST(TapeVsDual, EveryProductAndTreatmentAgreesPerPathWithinTheAssociationBound) {
  for (const std::uint32_t steps : {1U, 12U, 50U}) {
    for (const Case& c : AllCases(steps)) {
      ASSERT_EQ(pricer::Validate(c.contract, kMarket), pricer::ContractError::kNone) << c.name;
      Worst worst;
      CompareCase(c, worst);
      std::printf(
          "measured, %s, %u steps, %llu paths: max |tape - dual| / bound %.3g, / A %.3g, "
          "statements %zu\n",
          c.name.c_str(), steps, static_cast<unsigned long long>(kPaths), worst.ratio,
          worst.relative, worst.statements);
    }
  }
}

// The bound is not vacuous: one ULP of a Greek's own size in the wrong place is far outside it
// on a long path, so it sees an error of the size a wrong partial makes.
TEST(TapeVsDual, BoundIsFarBelowAWrongPartial) {
  const double bound = testing::AgreementBound(1.0, 5000, 8000);
  EXPECT_LT(bound, 1e-11);
  EXPECT_GT(bound, 0.0);
}

}  // namespace
}  // namespace aadp::ad
