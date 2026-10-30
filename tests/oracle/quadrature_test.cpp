// Pins BENCHMARKS.md section 9 against the committed tool. Derived values there were recomputed
// by the research scripts, so a match here is two independently written programs agreeing.
// Published tables: BGK 1997 Tables 2.1 and 2.4, BGK 1999 Table 1, Fusai and Meucci Table 6.
#include "quadrature.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "oracle/barrier.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/geometric_asian.hpp"

namespace aadp::quadrature {
namespace {

constexpr double kPin = 1e-9;  // BENCHMARKS.md b for O5, O6 and O9.

BarrierInputs Bgk(double barrier) { return {100, 100, barrier, 0.10, 0.0, 0.30, 0.2, 50}; }

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
constexpr BgkRow kBgkRows[] = {
    {85, 6.322, 0.591, 6.3222401405, 0.5909057930, 6.3075749325, 0.58049},
    {90, 6.098, 0.633, 6.0981961431, 0.6328096928, 5.9772419956, 0.54133},
    {95, 4.907, 0.798, 4.9067890354, 0.7978141916, 4.3975025600, 0.39101},
    {97, 3.834, 0.917, 3.8339777052, 0.9169759262, 3.0595633046, 0.28486},
    {99, 2.337, 0.958, 2.3363868958, 0.9578825906, 1.1707930349, 0.15960},
};

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
    const BarrierResult a = DiscreteDownOutGaussLegendre(Bgk(row.barrier), {3.0, 10, 14.0, 15.0});
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
  const BarrierInputs in{100, 100, 95, 0.10, 0.0, 0.60, 0.2, 4};
  const BarrierResult gl = DiscreteDownOutGaussLegendre(in, {});
  EXPECT_NEAR(gl.value, 9.4905, 1e-4);      // BENCHMARKS.md reference and b.
  EXPECT_NEAR(gl.value, 9.49052, 2e-5);     // BGK 1999 published value.
  EXPECT_NEAR(gl.value, 9.490535, 5.1e-7);  // crosscheck.py.
}

TEST(DiscreteBarrier, StrikeBelowBarrierAgreesAcrossMethods) {
  const BarrierInputs in{100, 90, 95, 0.05, 0.02, 0.30, 0.5, 12};
  const BarrierResult a = DiscreteDownOutGaussLegendre(in, {3.0, 10, 14.0, 15.0});
  const BarrierResult b = DiscreteDownOutSimpson(in, {});
  EXPECT_NEAR(a.value, b.value, kPin);
  EXPECT_NEAR(a.delta, b.delta, kPin);
}

// Fusai and Meucci's printed sigma 0.17801 does not reproduce their table. All the geometric
// rows match closed form only at sigma = 0.1780163, and the arithmetic rows then fall within
// 2e-5. BENCHMARKS.md keeps the printed sigma, so both positions are pinned here.
constexpr double kImpliedSigma = 0.1780163;

AsianInputs Fm(double strike, std::uint32_t steps, double sigma = 0.17801) {
  return {100, strike, 0.0367, 0.0, sigma, 1.0, steps};
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
  const Row rows[] = {{90, 12, 11.90497, 11.90498}, {100, 12, 4.88210, 4.88212},
                      {110, 12, 1.36314, 1.36314},  {90, 50, 11.93301, 11.93299},
                      {100, 50, 4.93736, 4.93738},  {110, 50, 1.40264, 1.40262}};
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
  const Row rows[] = {{90, 12, 11.66744},  {100, 12, 4.703634}, {110, 12, 1.251243},
                      {90, 50, 11.7076},   {100, 50, 4.769273}, {110, 50, 1.299134},
                      {100, 250, 4.787024}};
  for (const Row& row : rows) {
    const auto market = [&](double sigma) {
      return MarketParams<double>{100, sigma, 0.0367, 0, 1};
    };
    const auto value = [&](double sigma) {
      return GeometricAsianValue(OptionType::kCall, row.strike, row.steps, true, market(sigma));
    };
    EXPECT_NEAR(value(kImpliedSigma), row.published, 6e-6) << row.strike;
    EXPECT_GT(std::fabs(value(0.17801) - row.published), 4e-5) << row.strike;
  }
}

TEST(ArithmeticAsian, RecursionMatchesBruteForceAtThreeSteps) {
  for (double strike : {90.0, 100.0, 110.0}) {
    const AsianResult recursion = ArithmeticAsianCall(Fm(strike, 3), {6.0, 14.0, 16.0});
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
  const AsianInputs in{2.0, 2.0, 0.05, 0.0, 0.50, 1.0, 0};
  const std::vector<double> diagonal = ArithmeticAsianRichardson(in, 64, 4, {});
  ASSERT_EQ(diagonal.size(), 4U);
  const double raw = ArithmeticAsianCall({2.0, 2.0, 0.05, 0.0, 0.50, 1.0, 512}, {}).value;
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
  const Expected expected[] = {
      {"O1", 0.5946, 5.1e-5},       {"O2", 38.7325, 5.1e-5},      {"O3", -31.1924, 5.1e-5},
      {"O4", 5.3425606635, 1e-10},  {"O5", 4.9067890354, kPin},   {"O6", 0.7978141916, kPin},
      {"O7", 4.3975025600, kPin},   {"O8", 9.4905, 1e-4},         {"O9", 4.937202814482, kPin},
      {"O10", 0.2464156905, 1e-10}, {"O11", 1.3939809367, 1e-10},
  };
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
  std::vector<std::string> ids;
  for (const ReferenceRow& row : rows) {
    EXPECT_EQ(Find(rows, row.id), &row) << "duplicate " << row.id;
    for (const std::string* field : {&row.id, &row.parameters, &row.method, &row.source}) {
      EXPECT_EQ(field->find(','), std::string::npos) << *field;
    }
  }
}

}  // namespace
}  // namespace aadp::quadrature
