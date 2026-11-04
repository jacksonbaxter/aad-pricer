// Check 1: per path, on every pricer product and treatment, the tape's gradient against the
// forward-mode Dual of tests/support/ on the same kernel and the same uniforms.
//
// Bound, stated before the comparison ran: bounds.hpp, |tape - dual| <= 2 gamma_n A (1 + 2
// gamma_n) plus the underflow term, per input, with A the path's absolute gradient and
// n = 2 T + O from the path's own tape. (The underflow term was first n 2^-1070; bounds.hpp
// says why it changed. On these normal-range paths it is far below the first term.) The reason:
// both compute the same pathwise derivative from the same partials, in different association, and
// that bounds what association can change. Also checked: the tape's value is the double kernel's,
// bit for bit, as is the Dual's.
//
// Check 8: the hand-written adjoint of src/ad/hand_adjoint.hpp against the tape, per path and
// per batch, on the arithmetic Asian and the European. It lives here, beside check 1, because it
// is the same kind of identity. Bound, stated before measuring: the hand adjoint forms each term
// of the derivative from the same partial doubles as the tape (hand_adjoint.hpp lists them), so
// again only association differs. Its path part takes the tape's order, and its set-up adjoint
// forms the tape's products on the same two doubles and adds them in another order. So a term
// passes no more roundings than n = 2 T + O allows, and no product the tape does not also form,
// so the tape's underflow weight W covers it, and per path
//   |hand - tape| <= 2 gamma_n A (1 + 2 gamma_n) + 2^-1074 W (1 + 2 gamma_n) + 2^-1072,
// check 1's bound, AgreementBound in bounds.hpp. Per batch, both runs sum the same paths' terms
// over blocks and batches as the tape run in TapeThreads.ManyShortPathsReuseEachTapeAcrossBlocks
// does, so that test's bound holds between them too. The value must be Price()'s, bit for bit.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ad/bounds.hpp"
#include "ad/gradient.hpp"
#include "ad/hand_adjoint.hpp"
#include "cases.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/dual_ops.hpp"  // IWYU pragma: keep (tests/pricer/: the Dual overloads, by ADL)
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
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
    const auto value = pricer::DiscountedPayoff<double>(c.contract, model, u);
    const auto dual = pricer::DiscountedPayoff<test::Dual>(c.contract, dual_model, u);
    const PathGradient::PathResult result = tape.Evaluate(u);
    EXPECT_EQ(Bits(result.value), Bits(value)) << c.name << ", path " << path;
    EXPECT_EQ(Bits(dual.value), Bits(value)) << c.name << ", path " << path;
    worst.statements = std::max(worst.statements, result.statements);
    for (std::size_t i = 0; i < kInputs; ++i) {
      const double bound = bounds::AgreementBound(result.absolute[i], result.operand_sums[i],
                                                  result.statements, result.operands);
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
  const double bound = bounds::AgreementBound(1.0, 1e4, 5000, 8000);
  EXPECT_LT(bound, 1e-11);
  EXPECT_GT(bound, 0.0);
}

// Check 8, per path. Two markets, so rates, dividends and the drift change sign between them.
constexpr pricer::Market<double> kSecondMarket{
    .spot = 80.0, .sigma = 0.45, .r = -0.01, .q = 0.03, .maturity = 2.0};
constexpr std::array<const char*, kInputs> kGreekNames = {"delta", "vega", "rho", "dividend rho",
                                                          "theta"};

struct HandCase {
  std::string name;
  pricer::Contract contract;
};

std::vector<HandCase> HandCases(std::uint32_t steps) {
  using enum pricer::Product;
  using enum pricer::OptionType;
  return {
      {.name = "arithmetic Asian call with spot",
       .contract = testing::Plain(kAsianArithmetic, kCall, steps, true)},
      {.name = "arithmetic Asian call", .contract = testing::Plain(kAsianArithmetic, kCall, steps)},
      {.name = "arithmetic Asian put", .contract = testing::Plain(kAsianArithmetic, kPut, steps)},
      {.name = "arithmetic Asian put with spot",
       .contract = testing::Plain(kAsianArithmetic, kPut, steps, true)},
      {.name = "european call", .contract = testing::Plain(kEuropean, kCall, steps)},
      {.name = "european put", .contract = testing::Plain(kEuropean, kPut, steps)},
  };
}

struct HandTally {
  double worst = 0.0;  // max |hand - tape| / bound
  std::size_t paths = 0;
  std::array<std::size_t, kInputs> identical{};
};

void CompareHandCase(const HandCase& c, const pricer::Market<double>& market, HandTally& tally) {
  constexpr std::uint64_t kHandPaths = 256;
  const std::uint32_t steps = c.contract.steps;
  ASSERT_EQ(pricer::Validate(c.contract, market), pricer::ContractError::kNone) << c.name;
  const hand::Setup setup = hand::MakeSetup(c.contract, market);
  hand::Scratch scratch(steps);
  const pricer::GbmModel<double> model(market, steps);
  PathGradient tape(c.contract, market);
  std::vector<double> normal(steps);
  std::vector<double> crossing(steps);
  const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
  for (std::uint64_t path = 0; path < kHandPaths; ++path) {
    pricer::FillPath(kSeed, path, normal, crossing);
    hand::Boundary boundary;
    const double value = setup.asian ? hand::AsianPath(setup, normal, scratch, boundary)
                                     : hand::EuropeanPath(setup, normal, scratch, boundary);
    const std::array<double, kInputs> gradient = hand::MarketAdjoint(setup, boundary);
    const PathGradient::PathResult r = tape.Evaluate(u);
    ASSERT_EQ(Bits(value), Bits(pricer::DiscountedPayoff<double>(c.contract, model, u)))
        << c.name << ", " << steps << " steps, path " << path;
    tally.paths += 1;
    for (std::size_t i = 0; i < kInputs; ++i) {
      const double bound =
          bounds::AgreementBound(r.absolute[i], r.operand_sums[i], r.statements, r.operands);
      const double diff = std::fabs(gradient[i] - r.gradient[i]);
      ASSERT_LE(diff, bound) << c.name << ", " << steps << " steps, S0 " << market.spot << ", path "
                             << path << ", " << kGreekNames[i] << ": hand " << gradient[i]
                             << ", tape " << r.gradient[i];
      if (Bits(gradient[i]) == Bits(r.gradient[i])) tally.identical[i] += 1;
      if (diff > 0.0) tally.worst = std::max(tally.worst, diff / bound);
    }
  }
}

TEST(HandAdjoint, AgreesWithTheTapePerPathWithinTheAssociationBound) {
  HandTally tally;
  for (const pricer::Market<double>& market : {kMarket, kSecondMarket}) {
    for (const std::uint32_t steps : {1U, 12U, 50U, 252U}) {
      for (const HandCase& c : HandCases(steps)) {
        CompareHandCase(c, market, tally);
        if (HasFatalFailure()) return;
      }
    }
  }
  std::printf(
      "measured: hand adjoint against the tape on %zu paths, bit-identical: delta %zu, vega %zu, "
      "rho %zu, dividend rho %zu, theta %zu; worst |hand - tape| / bound %.3g\n",
      tally.paths, tally.identical[0], tally.identical[1], tally.identical[2], tally.identical[3],
      tally.identical[4], tally.worst);
}

// Check 8, per batch, on whole runs, with the bound of TapeThreads.ManyShortPathsReuse...
TEST(HandAdjoint, RunMatchesPriceBitForBitAndTheTapeBatchByBatch) {
  using enum pricer::Product;
  using enum pricer::OptionType;
  constexpr std::uint32_t kBatches = 4;
  constexpr std::uint64_t kPerBatch = (2 * pricer::kBlockPaths) + 5;
  constexpr std::uint64_t kBlocksPerBatch =
      (kPerBatch + pricer::kBlockPaths - 1) / pricer::kBlockPaths;
  const pricer::RunConfig config{
      .seed = kSeed, .paths = kBatches * kPerBatch, .batches = kBatches, .threads = 3};
  const std::vector<HandCase> cases = {
      {.name = "arithmetic Asian call with spot, 12 steps",
       .contract = testing::Plain(kAsianArithmetic, kCall, 12, true)},
      {.name = "arithmetic Asian put, 12 steps",
       .contract = testing::Plain(kAsianArithmetic, kPut, 12)},
      {.name = "european call, 12 steps", .contract = testing::Plain(kEuropean, kCall, 12)},
      {.name = "european put, 1 step", .contract = testing::Plain(kEuropean, kPut, 1)},
  };
  double worst = 0.0;
  for (const HandCase& c : cases) {
    const auto hand_run = hand::PriceWithHandAdjoint(c.contract, kMarket, config);
    const auto tape_run = PriceWithGradient(c.contract, kMarket, config);
    const auto price = pricer::Price(c.contract, kMarket, config);
    ASSERT_TRUE(hand_run.has_value() && tape_run.has_value() && price.has_value()) << c.name;
    EXPECT_EQ(Bits(hand_run->value.mean), Bits(price->mean)) << c.name;
    EXPECT_EQ(Bits(hand_run->value.standard_error), Bits(price->standard_error)) << c.name;
    PathGradient path(c.contract, kMarket);
    std::vector<double> normal(c.contract.steps);
    std::vector<double> crossing(c.contract.steps);
    const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
    const auto p = static_cast<double>(pricer::kBlockPaths);
    const auto n = static_cast<double>(kPerBatch);
    for (std::uint64_t b = 0; b < kBatches; ++b) {
      EXPECT_EQ(Bits(hand_run->value.batch_means[b]), Bits(price->batch_means[b]))
          << c.name << ", batch " << b;
      std::array<double, kInputs> bound{};
      for (std::uint64_t i = 0; i < kPerBatch; ++i) {
        pricer::FillPath(kSeed, (b * kPerBatch) + i, normal, crossing);
        const PathGradient::PathResult r = path.Evaluate(u);
        const double m = (2.0 * static_cast<double>(r.statements)) +
                         ((p + 2.0) * static_cast<double>(r.operands)) + n +
                         (2.0 * static_cast<double>(kBlocksPerBatch)) + 4.0;
        const double gamma = bounds::Gamma(m);
        for (std::size_t g = 0; g < kInputs; ++g) {
          bound[g] +=
              ((2.0 * gamma * r.absolute[g]) + (2.0 * bounds::UnderflowTerm(r.operand_sums[g]))) *
              (1.0 + (2.0 * gamma));
        }
      }
      for (std::size_t g = 0; g < kInputs; ++g) {
        const double h = hand_run->greeks[g].batch_means[b];
        const double t = tape_run->greeks[g].batch_means[b];
        const double limit = ((bound[g] / n) * (1.0 + 0x1p-50)) + bounds::kBoundRounding +
                             (0x1p-52 * (std::fabs(h) + std::fabs(t)));
        ASSERT_LE(std::fabs(h - t), limit) << c.name << ", batch " << b << ", " << kGreekNames[g]
                                           << ": hand " << h << ", tape " << t;
        if (h != t) worst = std::max(worst, std::fabs(h - t) / limit);
      }
    }
  }
  std::printf(
      "measured: hand adjoint run against the tape run, worst batch |hand - tape| / bound "
      "%.3g\n",
      worst);
}

TEST(HandAdjoint, RefusesWhatItDoesNotPrice) {
  using enum pricer::Product;
  using enum pricer::OptionType;
  const pricer::RunConfig config{.seed = kSeed, .paths = 64, .batches = 4, .threads = 1};
  for (const pricer::Contract& c :
       {testing::Plain(kDigital, kCall, 4), testing::Plain(kAsianGeometric, kCall, 4),
        testing::WithBarrier(pricer::Direction::kDown, pricer::Knock::kOut,
                             pricer::Monitoring::kDiscrete, pricer::Treatment::kOneStep, 4)}) {
    const auto refused = hand::PriceWithHandAdjoint(c, kMarket, config);
    ASSERT_FALSE(refused.has_value());
    EXPECT_TRUE(refused.error().unsupported_product);
  }
  const pricer::Contract asian = testing::Plain(kAsianArithmetic, kCall, 4);
  pricer::Market<double> bad = kMarket;
  bad.sigma = -1.0;
  const auto bad_market = hand::PriceWithHandAdjoint(asian, bad, config);
  ASSERT_FALSE(bad_market.has_value());
  EXPECT_EQ(bad_market.error().price.contract, pricer::ContractError::kVolatility);
  const auto bad_run =
      hand::PriceWithHandAdjoint(asian, kMarket, {.seed = kSeed, .paths = 64, .batches = 1});
  ASSERT_FALSE(bad_run.has_value());
  EXPECT_EQ(bad_run.error().price.run, pricer::RunError::kBatches);
  EXPECT_TRUE(hand::PriceWithHandAdjoint(asian, kMarket, config).has_value());
}

}  // namespace
}  // namespace aadp::ad
