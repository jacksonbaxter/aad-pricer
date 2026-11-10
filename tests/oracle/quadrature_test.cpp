// Pins BENCHMARKS.md section 9 against the committed tool. Derived values there were recomputed
// by the research scripts, so a match here is two independently written programs agreeing.
// Published tables: BGK 1997 Tables 2.1 and 2.4, BGK 1999 Table 1, Fusai and Meucci Table 6.
#include "quadrature.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "oracle/black_scholes.hpp"
#include "oracle/geometric_asian.hpp"
#include "support/dual.hpp"
#include "support/mc_support.hpp"

namespace aadp::quadrature {
namespace {

constexpr double kPin = 1e-9;  // BENCHMARKS.md b for O5, O6 and O9.

BarrierInputs Bgk(double barrier) {
  return {.spot = 100,
          .strike = 100,
          .barrier = barrier,
          .r = 0.10,
          .q = 0.0,
          .sigma = 0.30,
          .maturity = 0.2,
          .monitors = 50};
}

struct BgkRow {
  double barrier;
  double published_value;
  double published_delta;
  double value;
  double delta;
  double continuous;
  double naive;
};

// Value and delta to 10 decimals: BENCHMARKS.md and rerun/barrier.out. Naive: rerun/naive.out.
constexpr auto kBgkRows = std::to_array<BgkRow>({
    {.barrier = 85,
     .published_value = 6.322,
     .published_delta = 0.591,
     .value = 6.3222401405,
     .delta = 0.5909057930,
     .continuous = 6.3075749325,
     .naive = 0.58049},
    {.barrier = 90,
     .published_value = 6.098,
     .published_delta = 0.633,
     .value = 6.0981961431,
     .delta = 0.6328096928,
     .continuous = 5.9772419956,
     .naive = 0.54133},
    {.barrier = 95,
     .published_value = 4.907,
     .published_delta = 0.798,
     .value = 4.9067890354,
     .delta = 0.7978141916,
     .continuous = 4.3975025600,
     .naive = 0.39101},
    {.barrier = 97,
     .published_value = 3.834,
     .published_delta = 0.917,
     .value = 3.8339777052,
     .delta = 0.9169759262,
     .continuous = 3.0595633046,
     .naive = 0.28486},
    {.barrier = 99,
     .published_value = 2.337,
     .published_delta = 0.958,
     .value = 2.3363868958,
     .delta = 0.9578825906,
     .continuous = 1.1707930349,
     .naive = 0.15960},
});

TEST(DiscreteBarrier, PinsAllFiveRows) {
  for (const BgkRow& row : kBgkRows) {
    const BarrierResult result = DiscreteDownOutGaussLegendre(Bgk(row.barrier), {});
    EXPECT_NEAR(result.value, row.value, kPin) << row.barrier;
    EXPECT_NEAR(result.delta, row.delta, kPin) << row.barrier;
    EXPECT_NEAR(result.naive_delta, row.naive, 5.1e-6) << row.barrier;
  }
}

TEST(DiscreteBarrier, SimpsonAgreesWithGaussLegendre) {
  for (const BgkRow& row : kBgkRows) {
    const BarrierResult a = DiscreteDownOutGaussLegendre(
        Bgk(row.barrier), {.panels_per_sd = 3.0, .degree = 10, .top_sd = 14.0, .kernel_sd = 15.0});
    const BarrierResult b = DiscreteDownOutSimpson(Bgk(row.barrier), {});
    EXPECT_NEAR(a.value, b.value, kPin) << row.barrier;
    EXPECT_NEAR(a.delta, b.delta, kPin) << row.barrier;
    EXPECT_NEAR(a.naive_delta, b.naive_delta, kPin) << row.barrier;
  }
}

// Table rounding to 0.001 holds everywhere but H=99, where 2.3363868958 sits 6.1e-4 below the
// published 2.337, inside BGK's stated accuracy of 0.001. Stated position, so it is pinned.
TEST(DiscreteBarrier, PublishedRelativePositions) {
  for (const BgkRow& row : kBgkRows) {
    const BarrierResult r = DiscreteDownOutGaussLegendre(Bgk(row.barrier), {});
    EXPECT_NEAR(r.delta, row.published_delta, 5.0e-4) << row.barrier;
    if (row.barrier == 99) {
      const double gap = row.published_value - r.value;
      EXPECT_GT(gap, 6.0e-4);
      EXPECT_LT(gap, 6.2e-4);
    } else {
      EXPECT_NEAR(r.value, row.published_value, 5.0e-4) << row.barrier;
    }
  }
}

// The discrete and continuous contracts must stay apart, or the kernels could swap unnoticed.
TEST(DiscreteBarrier, IsAboveTheContinuousContract) {
  for (const BgkRow& row : kBgkRows) {
    const double continuous = ContinuousDownOutCallBgk(100, 100, row.barrier, 0.10, 0.0, 0.30, 0.2);
    EXPECT_NEAR(continuous, row.continuous, kPin) << row.barrier;
    EXPECT_GT(DiscreteDownOutGaussLegendre(Bgk(row.barrier), {}).value, continuous);
  }
}

TEST(DiscreteBarrier, O8FourMonitors) {
  const BarrierInputs in{.spot = 100,
                         .strike = 100,
                         .barrier = 95,
                         .r = 0.10,
                         .q = 0.0,
                         .sigma = 0.60,
                         .maturity = 0.2,
                         .monitors = 4};
  const BarrierResult gl = DiscreteDownOutGaussLegendre(in, {});
  EXPECT_NEAR(gl.value, 9.4905, 1e-4);      // BENCHMARKS.md reference and b.
  EXPECT_NEAR(gl.value, 9.49052, 2e-5);     // BGK 1999 published value.
  EXPECT_NEAR(gl.value, 9.490535, 5.1e-7);  // crosscheck.py.
}

TEST(DiscreteBarrier, StrikeBelowBarrierAgreesAcrossMethods) {
  const BarrierInputs in{.spot = 100,
                         .strike = 90,
                         .barrier = 95,
                         .r = 0.05,
                         .q = 0.02,
                         .sigma = 0.30,
                         .maturity = 0.5,
                         .monitors = 12};
  const BarrierResult a = DiscreteDownOutGaussLegendre(
      in, {.panels_per_sd = 3.0, .degree = 10, .top_sd = 14.0, .kernel_sd = 15.0});
  const BarrierResult b = DiscreteDownOutSimpson(in, {});
  EXPECT_NEAR(a.value, b.value, kPin);
  EXPECT_NEAR(a.delta, b.delta, kPin);
}

// Every pinned row above has q = 0, and the two methods share their log step, so they would agree
// on a drift with q missing. A barrier too far down to touch leaves the vanilla, and the vanilla's
// closed form has q of its own. Tolerance kPin, the b of the plan's discrete rows.
TEST(DiscreteBarrier, DividendEntersTheDrift) {
  const BarrierInputs in{.spot = 100,
                         .strike = 100,
                         .barrier = 20,
                         .r = 0.05,
                         .q = 0.03,
                         .sigma = 0.30,
                         .maturity = 0.5,
                         .monitors = 12};
  const auto vanilla = BlackScholes(
      OptionType::kCall, 100.0,
      MarketParams<double>{.spot = 100, .sigma = 0.30, .r = 0.05, .q = 0.03, .maturity = 0.5});
  for (const BarrierResult& result :
       {DiscreteDownOutGaussLegendre(in, {}), DiscreteDownOutSimpson(in, {})}) {
    EXPECT_NEAR(result.value, vanilla.value, kPin);
    EXPECT_NEAR(result.delta, vanilla.delta, kPin);
    EXPECT_NEAR(result.naive_delta, vanilla.delta, kPin);
  }
}

// Fusai and Meucci's printed sigma 0.17801 does not reproduce their table. All the geometric
// rows match closed form only at sigma = 0.1780163, and the arithmetic rows then fall within
// 2e-5. BENCHMARKS.md keeps the printed sigma, so both positions are pinned here.
constexpr double kImpliedSigma = 0.1780163;

AsianInputs Fm(double strike, std::uint32_t steps, double sigma = 0.17801) {
  return {.spot = 100,
          .strike = strike,
          .r = 0.0367,
          .q = 0.0,
          .sigma = sigma,
          .maturity = 1.0,
          .steps = steps};
}

TEST(ArithmeticAsian, O9) {
  EXPECT_NEAR(ArithmeticAsianCall(Fm(100, 50), {6.0, 14.0, 16.0}).value, 4.937202814482, kPin);
  EXPECT_NEAR(ArithmeticAsianCall(Fm(100, 50), {}).value, 4.937202814482, kPin);
}

TEST(ArithmeticAsian, PublishedSitsAboveThePrintedSigma) {
  const double gap = 4.93736 - ArithmeticAsianCall(Fm(100, 50), {}).value;
  EXPECT_GT(gap, 1.5e-4);
  EXPECT_LT(gap, 1.6e-4);
}

TEST(ArithmeticAsian, ImpliedSigmaReachesTheArithmeticColumns) {
  struct Row {
    double strike;
    std::uint32_t steps;
    double published_10000;  // Quadrature with 10000 nodes.
    double published_5000;   // Quadrature with 5000 nodes.
  };
  const auto rows = std::to_array<Row>(
      {{.strike = 90, .steps = 12, .published_10000 = 11.90497, .published_5000 = 11.90498},
       {.strike = 100, .steps = 12, .published_10000 = 4.88210, .published_5000 = 4.88212},
       {.strike = 110, .steps = 12, .published_10000 = 1.36314, .published_5000 = 1.36314},
       {.strike = 90, .steps = 50, .published_10000 = 11.93301, .published_5000 = 11.93299},
       {.strike = 100, .steps = 50, .published_10000 = 4.93736, .published_5000 = 4.93738},
       {.strike = 110, .steps = 50, .published_10000 = 1.40264, .published_5000 = 1.40262}});
  for (const Row& row : rows) {
    const double value = ArithmeticAsianCall(Fm(row.strike, row.steps, kImpliedSigma), {}).value;
    EXPECT_NEAR(value, row.published_10000, 2.0e-5) << row.strike << " " << row.steps;
    EXPECT_NEAR(value, row.published_5000, 4.0e-5) << row.strike << " " << row.steps;
  }
}

TEST(ArithmeticAsian, ImpliedSigmaReachesTheGeometricColumn) {
  struct Row {
    double strike;
    std::uint32_t steps;
    double published;
  };
  const auto rows = std::to_array<Row>({{.strike = 90, .steps = 12, .published = 11.66744},
                                        {.strike = 100, .steps = 12, .published = 4.703634},
                                        {.strike = 110, .steps = 12, .published = 1.251243},
                                        {.strike = 90, .steps = 50, .published = 11.7076},
                                        {.strike = 100, .steps = 50, .published = 4.769273},
                                        {.strike = 110, .steps = 50, .published = 1.299134},
                                        {.strike = 100, .steps = 250, .published = 4.787024}});
  for (const Row& row : rows) {
    const auto market = [&](double sigma) {
      return MarketParams<double>{.spot = 100, .sigma = sigma, .r = 0.0367, .q = 0, .maturity = 1};
    };
    const auto value = [&](double sigma) {
      return GeometricAsianValue(OptionType::kCall, row.strike, row.steps, true, market(sigma));
    };
    EXPECT_NEAR(value(kImpliedSigma), row.published, 6e-6) << row.strike;
    EXPECT_GT(std::fabs(value(0.17801) - row.published), 4e-5) << row.strike;
  }
}

TEST(ArithmeticAsian, RecursionMatchesBruteForceAtThreeSteps) {
  for (const double strike : {90.0, 100.0, 110.0}) {
    const AsianResult recursion = ArithmeticAsianCall(
        Fm(strike, 3), {.nodes_per_sd = 6.0, .grid_sd = 14.0, .kernel_sd = 16.0});
    EXPECT_NEAR(recursion.value, ArithmeticAsianBruteForce3(Fm(strike, 3)), 1e-12) << strike;
    EXPECT_NEAR(recursion.mass_error, 0.0, 1e-13);
    EXPECT_NEAR(recursion.martingale_error, 0.0, 1e-13);
  }
}

TEST(ArithmeticAsian, DividendEntersTheDrift) {
  // With q = r the undiscounted forward of every fixing is S0, and the moment check sees it.
  AsianInputs in = Fm(100, 12);
  in.q = in.r;
  const AsianResult with_yield = ArithmeticAsianCall(in, {});
  EXPECT_NEAR(with_yield.martingale_error, 0.0, 1e-13);
  EXPECT_LT(with_yield.value, ArithmeticAsianCall(Fm(100, 12), {}).value);
}

// New check, O10. Tolerance stated before the first run: 1e-7. Three levels on n = 64..512
// leave an error near c4 / (64 * 128 * 256 * 512), about c4 * 2e-10, so 1e-7 allows c4 up to
// 500. Linetsky's value is a continuous average, so the discrete tool reaches it only here.
TEST(ArithmeticAsian, RichardsonApproachesLinetskyCase5) {
  const AsianInputs in{
      .spot = 2.0, .strike = 2.0, .r = 0.05, .q = 0.0, .sigma = 0.50, .maturity = 1.0, .steps = 0};
  const std::vector<double> diagonal = ArithmeticAsianRichardson(in, 64, 4, {});
  ASSERT_EQ(diagonal.size(), 4U);
  const double raw = ArithmeticAsianCall({.spot = 2.0,
                                          .strike = 2.0,
                                          .r = 0.05,
                                          .q = 0.0,
                                          .sigma = 0.50,
                                          .maturity = 1.0,
                                          .steps = 512},
                                         {})
                         .value;
  for (std::size_t i = 0; i < diagonal.size(); ++i) {
    std::printf("Richardson level %zu: %.12f, error %.3e\n", i, diagonal[i],
                diagonal[i] - kLinetskyCase5);
  }
  std::printf("Raw n=512: %.12f, error %.3e\n", raw, raw - kLinetskyCase5);
  EXPECT_NEAR(diagonal.back(), kLinetskyCase5, kRichardsonTolerance);
  // Extrapolation must be doing the work: the plain value is far outside the tolerance.
  EXPECT_GT(std::fabs(raw - kLinetskyCase5), 100 * kRichardsonTolerance);
  EXPECT_LT(std::fabs(diagonal.back() - kLinetskyCase5), std::fabs(diagonal[1] - kLinetskyCase5));
}

const ReferenceRow* Find(const std::vector<ReferenceRow>& rows, const std::string& id) {
  for (const ReferenceRow& row : rows) {
    if (row.id == id) return &row;
  }
  return nullptr;
}

TEST(GeneratedReferences, CarrySectionNineValues) {
  std::vector<std::string> failures;
  const std::vector<ReferenceRow> rows = GenerateReferences(failures);
  EXPECT_TRUE(failures.empty());
  for (const std::string& failure : failures) ADD_FAILURE() << failure;
  struct Expected {
    const char* id;
    double value;
    double tolerance;
  };
  const auto expected = std::to_array<Expected>({
      {.id = "O1", .value = 0.5946, .tolerance = 5.1e-5},
      {.id = "O2", .value = 38.7325, .tolerance = 5.1e-5},
      {.id = "O3", .value = -31.1924, .tolerance = 5.1e-5},
      {.id = "O4", .value = 5.3425606635, .tolerance = 1e-10},
      {.id = "O5", .value = 4.9067890354, .tolerance = kPin},
      {.id = "O6", .value = 0.7978141916, .tolerance = kPin},
      {.id = "O7", .value = 4.3975025600, .tolerance = kPin},
      {.id = "O8", .value = 9.4905, .tolerance = 1e-4},
      {.id = "O9", .value = 4.937202814482, .tolerance = kPin},
      {.id = "O10", .value = 0.2464156905, .tolerance = 1e-10},
      {.id = "O11", .value = 1.3939809367, .tolerance = 1e-10},
  });
  for (const Expected& e : expected) {
    const ReferenceRow* row = Find(rows, e.id);
    ASSERT_NE(row, nullptr) << e.id;
    EXPECT_NEAR(row->value, e.value, e.tolerance) << e.id;
  }
  // BENCHMARKS.md's b, row by row.
  EXPECT_EQ(Find(rows, "O5")->b, 1e-9);
  EXPECT_EQ(Find(rows, "O8")->b, 1e-4);
  EXPECT_EQ(Find(rows, "O10")->b, 1e-10);
  EXPECT_EQ(Find(rows, "O7")->b, 0.0);
  EXPECT_EQ(Find(rows, "O11")->b, 0.0);
}

TEST(GeneratedReferences, IdsAreUniqueAndCsvIsWellFormed) {
  std::vector<std::string> failures;
  const std::vector<ReferenceRow> rows = GenerateReferences(failures);
  for (const ReferenceRow& row : rows) {
    EXPECT_EQ(Find(rows, row.id), &row) << "duplicate " << row.id;
    for (const std::string* field : {&row.id, &row.parameters, &row.method, &row.source}) {
      EXPECT_EQ(field->find(','), std::string::npos) << *field;
    }
  }
}

// Stage 8: the recursions over Dual (NOTES.md, "Stage 8").
MarketParams<test::Dual> KnockInActive() {
  return test::ActiveMarket({.spot = kKnockInTerms.spot,
                             .sigma = kKnockInTerms.sigma,
                             .r = kKnockInTerms.r,
                             .q = kKnockInTerms.q,
                             .maturity = kKnockInTerms.maturity});
}

// Dual's values are the double operations, so a template that changed the double path would
// show here as well as in the regenerated file.
TEST(DualRecursion, ValuesKeepTheDoubleBits) {
  const MarketParams<test::Dual> active = KnockInActive();
  for (const GaussLegendreResolution& resolution :
       {GaussLegendreResolution{}, kFineGaussLegendre}) {
    const BarrierResult plain = DiscreteDownOutGaussLegendre(kKnockInTerms, resolution);
    const DualBarrierResult dual = DiscreteDownOutGaussLegendre(kKnockInTerms, active, resolution);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(plain.value),
              std::bit_cast<std::uint64_t>(dual.value.value));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(plain.delta),
              std::bit_cast<std::uint64_t>(dual.delta.value));
    EXPECT_EQ(plain.nodes, dual.nodes);
  }
  const BarrierResult plain = DiscreteDownOutSimpson(kKnockInTerms, {});
  const DualBarrierResult dual = DiscreteDownOutSimpson(kKnockInTerms, active, {});
  EXPECT_EQ(std::bit_cast<std::uint64_t>(plain.value),
            std::bit_cast<std::uint64_t>(dual.value.value));
}

// The root's log spot carries the spot tangent, so the value's spot tangent is a second delta.
// Bound: kKnockInValueB, O6's b, as registered.
TEST(DualRecursion, SpotTangentIsTheScoreDelta) {
  const DualBarrierResult dual =
      DiscreteDownOutGaussLegendre(kKnockInTerms, KnockInActive(), kFineGaussLegendre);
  EXPECT_NEAR(dual.value.tangent[test::kSlotSpot], 0.7978141916, kPin);  // O6, pinned above.
  EXPECT_NEAR(dual.value.tangent[test::kSlotSpot], dual.delta.value, kKnockInValueB);
}

// A check the registration does not need, written apart from the tool: each tangent against a
// central difference of the double recursion, whose grid moves with the bump. Step 1e-4 of each
// input (2e-4 of spot's scale is 1e-2): truncation ~ h^2 times third derivatives of order 10^3 is
// below 1e-5, and the recursion's own error of 1e-12 over 2h adds 1e-8. So 1e-5 catches a wrong
// sign or a dropped tangent (each Greek here is above 0.1), not an error in the tenth digit.
TEST(DualRecursion, TangentsAgreeWithDifferencesOfTheDoubleRecursion) {
  const DualBarrierResult dual =
      DiscreteDownOutGaussLegendre(kKnockInTerms, KnockInActive(), kFineGaussLegendre);
  const auto value = [](BarrierInputs in) {
    return DiscreteDownOutGaussLegendre(in, kFineGaussLegendre).value;
  };
  const auto difference = [&](double BarrierInputs::* field, double h) {
    BarrierInputs up = kKnockInTerms;
    BarrierInputs down = kKnockInTerms;
    up.*field += h;
    down.*field -= h;
    return (value(up) - value(down)) / (2.0 * h);
  };
  EXPECT_NEAR(dual.value.tangent[test::kSlotSpot], difference(&BarrierInputs::spot, 1e-2), 1e-5);
  EXPECT_NEAR(dual.value.tangent[test::kSlotSigma], difference(&BarrierInputs::sigma, 1e-4), 1e-5);
  EXPECT_NEAR(dual.value.tangent[test::kSlotR], difference(&BarrierInputs::r, 1e-4), 1e-5);
  EXPECT_NEAR(dual.value.tangent[test::kSlotQ], difference(&BarrierInputs::q, 1e-4), 1e-5);
  EXPECT_NEAR(dual.value.tangent[test::kSlotMaturity], difference(&BarrierInputs::maturity, 1e-4),
              1e-5);
}

TEST(KnockInReferences, TwelveRowsAppendedWithTheirB) {
  std::vector<std::string> failures;
  const std::vector<ReferenceRow> rows = GenerateReferences(failures);
  EXPECT_TRUE(failures.empty());
  for (const std::string& failure : failures) ADD_FAILURE() << failure;
  ASSERT_GE(rows.size(), 12U);
  const std::string parameters =
      "S0=100;K=100;H=95;sigma=0.3;r=0.1;q=0;T=0.2;m=50;type=call;spot=0";
  std::size_t i = rows.size() - 12;
  EXPECT_EQ(rows[i - 1].id, "O11");
  for (const char* kind : {"KID", "KIC"}) {
    for (const char* greek : {"value", "delta", "vega", "rho", "divrho", "theta"}) {
      const ReferenceRow& row = rows[i++];
      EXPECT_EQ(row.id, std::string("P3.") + kind + "." + greek);
      EXPECT_EQ(row.parameters, parameters) << row.id;
      const bool discrete = std::string(kind) == "KID";
      const bool first_two = std::string(greek) == "value" || std::string(greek) == "delta";
      EXPECT_EQ(row.b, discrete ? (first_two ? 1e-9 : 1e-8) : 0.0) << row.id;
    }
  }
  // The knock-in is vanilla minus the knock-out on the same terms, whose values are pinned above.
  const double vanilla =
      BlackScholes(
          OptionType::kCall, 100.0,
          MarketParams<double>{.spot = 100, .sigma = 0.3, .r = 0.1, .q = 0.0, .maturity = 0.2})
          .value;
  EXPECT_NEAR(Find(rows, "P3.KID.value")->value, vanilla - 4.9067890354, kPin);
  EXPECT_NEAR(Find(rows, "P3.KIC.value")->value, vanilla - 4.3975025600, kPin);
  // Discrete monitoring knocks in less often than continuous.
  EXPECT_LT(Find(rows, "P3.KID.value")->value, Find(rows, "P3.KIC.value")->value);
}

}  // namespace
}  // namespace aadp::quadrature
