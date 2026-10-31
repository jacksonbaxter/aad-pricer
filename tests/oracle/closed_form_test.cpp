// Closed forms against published values. Sources, all read as data:
// - O1 to O3 and the other Greek rows: Haug 1998 pp. 11-16, as carried by QuantLib 5d6defcc
//   test-suite/europeanoption.cpp, testGreekValues.
// - O4: QuantLib 5d6defcc test-suite/asianoptions.cpp:429 (5.3425606635).
// - Barrier table: Haug 1998 p. 72, as carried by QuantLib 5d6defcc
//   test-suite/barrieroption.cpp, testHaugValues. The 48 European rows with H != S that
//   crosscheck.py checked. The other 24 have the spot on the barrier.
// - Beaglehole 5.477 and Babsiri rows: crosscheck.py, section 4.
// - O7: BGK 1997 Table 2.1, 4.398, closed form 4.3975025600.
#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "oracle/barrier.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/geometric_asian.hpp"
#include "support/dual.hpp"

namespace aadp {
namespace {

using test::Dual;

constexpr double kFourDecimals = 5.1e-5;  // Published to 4 decimals, half a unit plus rounding.

MarketParams<double> Market(double spot, double sigma, double r, double q, double maturity) {
  return {.spot = spot, .sigma = sigma, .r = r, .q = q, .maturity = maturity};
}

TEST(BlackScholes, O1DeltaCall) {
  const auto g = BlackScholes(OptionType::kCall, 100.0, Market(105, 0.36, 0.10, 0.10, 0.5));
  EXPECT_NEAR(g.delta, 0.5946, kFourDecimals);
}

TEST(BlackScholes, DeltaPut) {
  const auto g = BlackScholes(OptionType::kPut, 100.0, Market(105, 0.36, 0.10, 0.10, 0.5));
  EXPECT_NEAR(g.delta, -0.3566, kFourDecimals);
}

TEST(BlackScholes, O2RhoCall) {
  const auto g = BlackScholes(OptionType::kCall, 75.0, Market(72, 0.19, 0.09, 0.0, 1.0));
  EXPECT_NEAR(g.rho, 38.7325, kFourDecimals);
}

TEST(BlackScholes, O3ThetaPutPerYear) {
  const auto g = BlackScholes(OptionType::kPut, 405.0, Market(430, 0.20, 0.07, 0.05, 1.0 / 12.0));
  EXPECT_NEAR(g.theta, -31.1924, kFourDecimals);
  EXPECT_NEAR(g.theta / 365.0, -0.0855, kFourDecimals);
}

TEST(BlackScholes, GammaAndVega) {
  for (const OptionType type : {OptionType::kCall, OptionType::kPut}) {
    const auto g = BlackScholes(type, 60.0, Market(55, 0.30, 0.10, 0.0, 0.75));
    EXPECT_NEAR(g.gamma, 0.0278, kFourDecimals);
    EXPECT_NEAR(g.vega, 18.9358, kFourDecimals);
  }
}

TEST(BlackScholes, DividendRhoPut) {
  const auto g = BlackScholes(OptionType::kPut, 490.0, Market(500, 0.15, 0.08, 0.05, 0.25));
  EXPECT_NEAR(g.dividend_rho, 42.2254, kFourDecimals);
}

TEST(BlackScholes, PutCallParity) {
  const auto m = Market(105, 0.36, 0.07, 0.02, 0.5);
  const auto call = BlackScholes(OptionType::kCall, 100.0, m);
  const auto put = BlackScholes(OptionType::kPut, 100.0, m);
  EXPECT_NEAR(call.value - put.value, 105 * std::exp(-0.02 * 0.5) - 100 * std::exp(-0.07 * 0.5),
              1e-12);
}

// Hand-written Greeks against the chain rule through the value, so a sign or convention slip in
// either has to appear in both to survive. Theta is -dV/dT by construction here.
TEST(BlackScholes, HandGreeksMatchDualOnValue) {
  for (const OptionType type : {OptionType::kCall, OptionType::kPut}) {
    const MarketParams<Dual> m{.spot = Dual::Variable(105, 0),
                               .sigma = Dual::Variable(0.36, 1),
                               .r = Dual::Variable(0.07, 2),
                               .q = Dual::Variable(0.02, 3),
                               .maturity = Dual::Variable(0.5, 4)};
    const Dual value = BlackScholes(type, 100.0, m).value;
    const auto g = BlackScholes(type, 100.0, Market(105, 0.36, 0.07, 0.02, 0.5));
    EXPECT_NEAR(value.value, g.value, 1e-13);
    EXPECT_NEAR(value.tangent[0], g.delta, 1e-13);
    EXPECT_NEAR(value.tangent[1], g.vega, 1e-12);
    EXPECT_NEAR(value.tangent[2], g.rho, 1e-12);
    EXPECT_NEAR(value.tangent[3], g.dividend_rho, 1e-12);
    EXPECT_NEAR(-value.tangent[4], g.theta, 1e-12);
  }
}

TEST(GeometricAsian, O4TenFixings) {
  EXPECT_NEAR(
      GeometricAsianValue(OptionType::kCall, 100.0, 10, false, Market(100, 0.20, 0.06, 0.03, 1.0)),
      5.3425606635, 1e-10);
}

// The sums in the closed form replaced by the sums they stand for, with and without t = 0.
TEST(GeometricAsian, MatchesExplicitCovarianceSum) {
  const double spot = 100;
  const double strike = 95;
  const double sigma = 0.25;
  const double r = 0.04;
  const double q = 0.01;
  const double maturity = 0.75;
  for (const bool include_spot : {false, true}) {
    for (const std::uint32_t n : {1U, 2U, 7U, 50U}) {
      std::vector<double> times;
      if (include_spot) times.push_back(0.0);
      for (std::uint32_t i = 1; i <= n; ++i) times.push_back(maturity * i / n);
      double sum_t = 0.0;
      double sum_min = 0.0;
      for (const double a : times) {
        sum_t += a;
        for (const double b : times) sum_min += std::min(a, b);
      }
      const auto count = static_cast<double>(times.size());
      const double mean = std::log(spot) + (r - q - 0.5 * sigma * sigma) * sum_t / count;
      const double variance = sigma * sigma * sum_min / (count * count);
      const double sd = std::sqrt(variance);
      const double d2 = (mean - std::log(strike)) / sd;
      const double expected =
          std::exp(-r * maturity) *
          (std::exp(mean + 0.5 * variance) * normal_cdf(d2 + sd) - strike * normal_cdf(d2));
      EXPECT_NEAR(GeometricAsianValue(OptionType::kCall, strike, n, include_spot,
                                      Market(spot, sigma, r, q, maturity)),
                  expected, 1e-11)
          << n << " " << include_spot;
    }
  }
}

TEST(GeometricAsian, OneFixingWithoutSpotIsVanilla) {
  const auto m = Market(100, 0.20, 0.06, 0.03, 1.0);
  for (const OptionType type : {OptionType::kCall, OptionType::kPut}) {
    EXPECT_NEAR(GeometricAsianValue(type, 95.0, 1, false, m), BlackScholes(type, 95.0, m).value,
                1e-11);
  }
}

struct HaugRow {
  BarrierType barrier_type;
  OptionType option_type;
  double barrier;
  double strike;
  double sigma;
  double expected;
};

// Common to the table: S = 100, q = 0.04, r = 0.08, T = 0.5, rebate = 3.
constexpr auto kHaugRows = std::to_array<HaugRow>({
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 90.0,
     .sigma = 0.25,
     .expected = 9.0246},
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 100.0,
     .sigma = 0.25,
     .expected = 6.7924},
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 110.0,
     .sigma = 0.25,
     .expected = 4.8759},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 90.0,
     .sigma = 0.25,
     .expected = 2.6789},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 100.0,
     .sigma = 0.25,
     .expected = 2.3580},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 110.0,
     .sigma = 0.25,
     .expected = 2.3453},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 90.0,
     .sigma = 0.25,
     .expected = 7.7627},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 100.0,
     .sigma = 0.25,
     .expected = 4.0109},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 110.0,
     .sigma = 0.25,
     .expected = 2.0576},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 90.0,
     .sigma = 0.25,
     .expected = 14.1112},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 100.0,
     .sigma = 0.25,
     .expected = 8.4482},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 110.0,
     .sigma = 0.25,
     .expected = 4.5910},
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 90.0,
     .sigma = 0.30,
     .expected = 8.8334},
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 100.0,
     .sigma = 0.30,
     .expected = 7.0285},
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 110.0,
     .sigma = 0.30,
     .expected = 5.4137},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 90.0,
     .sigma = 0.30,
     .expected = 2.6341},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 100.0,
     .sigma = 0.30,
     .expected = 2.4389},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 110.0,
     .sigma = 0.30,
     .expected = 2.4315},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 90.0,
     .sigma = 0.30,
     .expected = 9.0093},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 100.0,
     .sigma = 0.30,
     .expected = 5.1370},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kCall,
     .barrier = 95.0,
     .strike = 110.0,
     .sigma = 0.30,
     .expected = 2.8517},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 90.0,
     .sigma = 0.30,
     .expected = 15.2098},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 100.0,
     .sigma = 0.30,
     .expected = 9.7278},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kCall,
     .barrier = 105.0,
     .strike = 110.0,
     .sigma = 0.30,
     .expected = 5.8350},
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 90.0,
     .sigma = 0.25,
     .expected = 2.2798},
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 100.0,
     .sigma = 0.25,
     .expected = 2.2947},
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 110.0,
     .sigma = 0.25,
     .expected = 2.6252},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 90.0,
     .sigma = 0.25,
     .expected = 3.7760},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 100.0,
     .sigma = 0.25,
     .expected = 5.4932},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 110.0,
     .sigma = 0.25,
     .expected = 7.5187},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 90.0,
     .sigma = 0.25,
     .expected = 2.9586},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 100.0,
     .sigma = 0.25,
     .expected = 6.5677},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 110.0,
     .sigma = 0.25,
     .expected = 11.9752},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 90.0,
     .sigma = 0.25,
     .expected = 1.4653},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 100.0,
     .sigma = 0.25,
     .expected = 3.3721},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 110.0,
     .sigma = 0.25,
     .expected = 7.0846},
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 90.0,
     .sigma = 0.30,
     .expected = 2.4170},
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 100.0,
     .sigma = 0.30,
     .expected = 2.4258},
    {.barrier_type = BarrierType::kDownOut,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 110.0,
     .sigma = 0.30,
     .expected = 2.6246},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 90.0,
     .sigma = 0.30,
     .expected = 4.2293},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 100.0,
     .sigma = 0.30,
     .expected = 5.8032},
    {.barrier_type = BarrierType::kUpOut,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 110.0,
     .sigma = 0.30,
     .expected = 7.5649},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 90.0,
     .sigma = 0.30,
     .expected = 3.8769},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 100.0,
     .sigma = 0.30,
     .expected = 7.7989},
    {.barrier_type = BarrierType::kDownIn,
     .option_type = OptionType::kPut,
     .barrier = 95.0,
     .strike = 110.0,
     .sigma = 0.30,
     .expected = 13.3078},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 90.0,
     .sigma = 0.30,
     .expected = 2.0658},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 100.0,
     .sigma = 0.30,
     .expected = 4.4226},
    {.barrier_type = BarrierType::kUpIn,
     .option_type = OptionType::kPut,
     .barrier = 105.0,
     .strike = 110.0,
     .sigma = 0.30,
     .expected = 8.3686},
});

TEST(BarrierClosedForm, HaugTable) {
  ASSERT_EQ(std::size(kHaugRows), 48U);
  double worst = 0.0;
  for (const HaugRow& row : kHaugRows) {
    const double value =
        BarrierOptionValue(row.barrier_type, row.option_type, row.strike, row.barrier, 3.0,
                           Market(100, row.sigma, 0.08, 0.04, 0.5));
    const double error = std::fabs(value - row.expected);
    worst = std::max(worst, error);
    EXPECT_LE(error, 1e-4) << static_cast<int>(row.barrier_type) << " "
                           << static_cast<int>(row.option_type) << " H=" << row.barrier
                           << " K=" << row.strike << " v=" << row.sigma;
  }
  // crosscheck.py: worst 6.3e-5, at the UpOut put H=105 K=90 v=0.30 row.
  EXPECT_LT(worst, 7e-5);
}

TEST(BarrierClosedForm, O7ContinuousDownAndOut) {
  const double value = BarrierOptionValue(BarrierType::kDownOut, OptionType::kCall, 100.0, 95.0,
                                          0.0, Market(100, 0.30, 0.10, 0.0, 0.2));
  EXPECT_NEAR(value, 4.3975025600, 1e-10);
}

TEST(BarrierClosedForm, BeagleholeAndBabsiri) {
  EXPECT_NEAR(BarrierOptionValue(BarrierType::kDownOut, OptionType::kCall, 50.0, 45.0, 0.0,
                                 Market(50, 0.5, std::log(1.1), 0.0, 1.0)),
              5.477, 5e-4);
  const auto babsiri = [](BarrierType type, double sigma, double barrier) {
    return BarrierOptionValue(type, OptionType::kCall, 100.0, barrier, 0.0,
                              Market(100, sigma, 0.05, 0.02, 1.0));
  };
  EXPECT_NEAR(babsiri(BarrierType::kDownIn, 0.1, 90), 0.07187, 5.1e-6);
  EXPECT_NEAR(babsiri(BarrierType::kDownIn, 0.3, 90), 4.50952, 5.1e-6);
  EXPECT_NEAR(babsiri(BarrierType::kUpIn, 0.1, 110), 4.79148, 5.1e-6);
  EXPECT_NEAR(babsiri(BarrierType::kUpIn, 0.3, 110), 12.98351, 5.1e-6);
}

// Without a rebate, in plus out is vanilla, in all eight cases and on both sides of the strike.
// This is the identity the Phase 3 knock-in parity rows rest on.
TEST(BarrierClosedForm, InOutParity) {
  const auto m = Market(100, 0.25, 0.08, 0.04, 0.5);
  for (OptionType option : {OptionType::kCall, OptionType::kPut}) {
    for (double strike : {90.0, 100.0, 110.0}) {
      const double vanilla = BlackScholes(option, strike, m).value;
      const auto value = [&](BarrierType type, double barrier) {
        return BarrierOptionValue(type, option, strike, barrier, 0.0, m);
      };
      EXPECT_NEAR(value(BarrierType::kDownIn, 95.0) + value(BarrierType::kDownOut, 95.0), vanilla,
                  1e-11);
      EXPECT_NEAR(value(BarrierType::kUpIn, 105.0) + value(BarrierType::kUpOut, 105.0), vanilla,
                  1e-11);
    }
  }
}

TEST(BarrierClosedForm, FarBarrierConvergesToVanilla) {
  const auto m = Market(100, 0.25, 0.08, 0.04, 0.5);
  EXPECT_NEAR(BarrierOptionValue(BarrierType::kDownOut, OptionType::kCall, 100.0, 1.0, 0.0, m),
              BlackScholes(OptionType::kCall, 100.0, m).value, 1e-9);
  EXPECT_NEAR(BarrierOptionValue(BarrierType::kUpOut, OptionType::kPut, 100.0, 1e4, 0.0, m),
              BlackScholes(OptionType::kPut, 100.0, m).value, 1e-9);
}

// The Greeks in the plan come from Dual through these closed forms, so check that route against
// deterministic central differences, one parameter at a time.
template <class Value>
void ExpectGreeksMatchCentralDifference(const MarketParams<double>& m, Value value, double h) {
  const MarketParams<Dual> active{.spot = Dual::Variable(m.spot, 0),
                                  .sigma = Dual::Variable(m.sigma, 1),
                                  .r = Dual::Variable(m.r, 2),
                                  .q = Dual::Variable(m.q, 3),
                                  .maturity = Dual::Variable(m.maturity, 4)};
  const Dual result = value(active);
  EXPECT_NEAR(result.value, value(m), 1e-12);
  const auto bumped = [&](int slot, double bump) {
    MarketParams<double> b = m;
    (slot == 0   ? b.spot
     : slot == 1 ? b.sigma
     : slot == 2 ? b.r
     : slot == 3 ? b.q
                 : b.maturity) += bump;
    return value(b);
  };
  for (int slot = 0; slot < 5; ++slot) {
    const double difference = (bumped(slot, h) - bumped(slot, -h)) / (2.0 * h);
    EXPECT_NEAR(result.tangent[static_cast<std::size_t>(slot)], difference,
                5e-7 * (1.0 + std::fabs(difference)))
        << "slot " << slot;
  }
}

TEST(ClosedFormGreeks, DualMatchesDeterministicDifferences) {
  const auto m = Market(100, 0.30, 0.10, 0.02, 0.5);
  ExpectGreeksMatchCentralDifference(
      m,
      [](const auto& p) {
        return BarrierOptionValue(BarrierType::kDownOut, OptionType::kCall, 100.0, 95.0, 0.0, p);
      },
      1e-5);
  ExpectGreeksMatchCentralDifference(
      m,
      [](const auto& p) {
        return BarrierOptionValue(BarrierType::kDownOut, OptionType::kCall, 90.0, 95.0, 0.0, p);
      },
      1e-5);
  ExpectGreeksMatchCentralDifference(
      m, [](const auto& p) { return GeometricAsianValue(OptionType::kCall, 100.0, 10, false, p); },
      1e-5);
}

}  // namespace
}  // namespace aadp
