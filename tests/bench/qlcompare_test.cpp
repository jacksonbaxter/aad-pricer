// The QuantLib comparison's own side (bench/quantlib/): the 42 comparisons as pre-registered, each
// contract's mapping onto the pricer, the references and R9's allowances, R3's control variate,
// lanes against scalar, the local-volatility grid rule, the ceilings file and the metrics. No
// QuantLib here; its side is checked by the bench's smoke run in the quantlib preset.
#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "oracle/black_scholes.hpp"
#include "oracle/references.hpp"
#include "pricer/contract.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "quadrature.hpp"
#include "quantlib/compare.hpp"
#include "quantlib/local_vol_rows.hpp"
#include "quantlib/ours.hpp"
#include "svi/raw_svi.hpp"

namespace aadp::qlcompare {
namespace {

ReferenceTable LoadTable() {
  const std::ifstream file("references/oracle_values.csv", std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  auto table = DecodeReferences(text.str());
  EXPECT_TRUE(table.has_value());
  return table ? *table : ReferenceTable{};
}

Case CaseById(const std::vector<Case>& cases, const std::string& id) {
  for (const Case& c : cases) {
    if (c.id == id) return c;
  }
  ADD_FAILURE() << "no case " << id;
  return cases.front();
}

TEST(QlCompareRows, FortyTwoComparisonsAsRegistered) {
  const std::vector<Comparison> all = Comparisons();
  ASSERT_EQ(all.size(), kComparisonCount);
  std::map<Row, int> per_row;
  std::set<std::string> names;
  for (const Comparison& c : all) {
    ++per_row[c.contract.row];
    EXPECT_TRUE(names.insert(c.Name()).second) << c.Name();
    EXPECT_FALSE(c.contract.row == Row::kR6 && c.engine == Engine::kQuantLib);
  }
  EXPECT_EQ(per_row[Row::kR1], 2);
  EXPECT_EQ(per_row[Row::kR2], 6);
  EXPECT_EQ(per_row[Row::kR3], 2);
  EXPECT_EQ(per_row[Row::kR4], 2);
  EXPECT_EQ(per_row[Row::kR5], 10);
  EXPECT_EQ(per_row[Row::kR6], 5);
  EXPECT_EQ(per_row[Row::kR7], 10);
  EXPECT_EQ(per_row[Row::kR9a], 2);
  EXPECT_EQ(per_row[Row::kR9b], 3);
  EXPECT_NEAR(Critical(), 3.8975749765, 1e-10);
}

TEST(QlCompareRows, ContractsMapOntoThePricer) {
  const std::vector<Case> cases = AccuracyCases();
  const pricer::Contract r1 = OurContract(CaseById(cases, "R1"), Engine::kOurs);
  EXPECT_EQ(r1.product, pricer::Product::kAsianGeometric);
  EXPECT_EQ(r1.steps, 10U);
  EXPECT_FALSE(r1.include_spot);
  const pricer::Market<double> m1 = OurMarket(CaseById(cases, "R1"));
  EXPECT_EQ(m1.r, 0.06);
  EXPECT_EQ(m1.q, 0.03);

  const pricer::Contract r2 = OurContract(CaseById(cases, "R2.K090"), Engine::kOurs);
  EXPECT_EQ(r2.product, pricer::Product::kAsianArithmetic);
  EXPECT_EQ(r2.steps, 12U);
  EXPECT_TRUE(r2.include_spot);
  EXPECT_EQ(r2.strike, 90.0);
  EXPECT_EQ(OurContract(CaseById(cases, "R4"), Engine::kOurs).steps, 50U);
  EXPECT_EQ(OurMarket(CaseById(cases, "R4")).sigma, 0.17801);

  const pricer::Contract r5 = OurContract(CaseById(cases, "R5.H090"), Engine::kOurs);
  EXPECT_EQ(r5.barrier.monitoring, pricer::Monitoring::kContinuous);
  EXPECT_EQ(r5.barrier.treatment, pricer::Treatment::kHard);
  EXPECT_EQ(r5.barrier.level, 90.0);
  const pricer::Contract r6 = OurContract(CaseById(cases, "R6.H090"), Engine::kOurs);
  EXPECT_EQ(r6.barrier.monitoring, pricer::Monitoring::kContinuous);
  EXPECT_EQ(r6.barrier.treatment, pricer::Treatment::kBridge);
  const pricer::Contract r7 = OurContract(CaseById(cases, "R7.H097"), Engine::kOurs);
  EXPECT_EQ(r7.barrier.monitoring, pricer::Monitoring::kDiscrete);
  EXPECT_EQ(r7.barrier.treatment, pricer::Treatment::kHard);
  EXPECT_EQ(r7.steps, 50U);
  const pricer::Market<double> m7 = OurMarket(CaseById(cases, "R7.H097"));
  EXPECT_EQ(m7.maturity, 0.2);
  EXPECT_EQ(m7.sigma, 0.30);

  const Case r9b = CaseById(cases, "R9b");
  EXPECT_EQ(OurContract(r9b, Engine::kOursOneStep).barrier.treatment, pricer::Treatment::kOneStep);
  const pricer::Contract smoothed = OurContract(r9b, Engine::kOursSmoothed);
  EXPECT_EQ(smoothed.barrier.treatment, pricer::Treatment::kSmoothed);
  EXPECT_EQ(smoothed.barrier.smoothing_width, 0.005);
  for (const Comparison& c : Comparisons()) {
    if (c.engine == Engine::kQuantLib) continue;
    EXPECT_EQ(pricer::Validate(OurContract(c.contract, c.engine), OurMarket(c.contract)),
              pricer::ContractError::kNone)
        << c.Name();
  }
}

TEST(QlCompareReferences, EveryComparisonHasItsReference) {
  const ReferenceTable table = LoadTable();
  const R9References r9 = ComputeR9References(table);
  std::map<std::string, RowReference> refs;
  for (const Comparison& c : Comparisons()) {
    const auto ref = ReferenceFor(c, table, r9);
    ASSERT_TRUE(ref.has_value()) << c.Name();
    refs[c.Name()] = *ref;
  }
  EXPECT_EQ(refs["R1/quantlib"].value, 5.342560663499);
  EXPECT_EQ(refs["R2.K110/ours"].value, 1.363037952856);
  EXPECT_EQ(refs["R2.K110/ours"].b, 1e-9);
  EXPECT_EQ(refs["R4/ours"].value, 4.937202814482);
  EXPECT_EQ(refs["R7.H095/quantlib"].value, 4.906789035411);
  EXPECT_EQ(refs["R7.H099/ours"].value, 2.336386895814);
  EXPECT_EQ(refs["R5.H095/ours"].value, 4.397502559963);
  // H 90, 97, 99 by closed form, against Phase 1's E32 and the quadrature tool's own BGK form.
  EXPECT_NEAR(refs["R6.H090/ours"].value, 5.9772419956, 1e-10);
  EXPECT_NEAR(refs["R5.H097/quantlib"].value, 3.0595633046, 1e-10);
  EXPECT_NEAR(refs["R5.H099/ours"].value, 1.1707930349, 1e-10);
  EXPECT_NEAR(refs["R5.H099/ours"].value,
              quadrature::ContinuousDownOutCallBgk(100, 100, 99, 0.10, 0.0, 0.30, 0.2), 1e-12);
  EXPECT_EQ(refs["R5.H090/ours"].b, 0.0);
  EXPECT_EQ(refs["R9b/ours-one-step"].value, 0.797814191618);
  EXPECT_EQ(refs["R9b/ours-one-step"].b, 1e-9);
  EXPECT_GT(refs["R9b/quantlib"].b, std::fabs(r9.barrier_cd - r9.barrier_delta));
  EXPECT_LT(std::fabs(r9.barrier_cd - r9.barrier_delta), 0.01);
  EXPECT_LT(r9.asian_b, 1e-6);
  EXPECT_LT(std::fabs(r9.asian_cd_2h - r9.asian_delta), 1e-6);
}

// The tape's pathwise delta must find R9a's reference: a wrong bump in the reference shows.
TEST(QlCompareReferences, R9aReferenceAgreesWithTheTape) {
  const R9References r9 = ComputeR9References(LoadTable());
  const std::vector<Case> cases = AccuracyCases();
  const Comparison c{.contract = CaseById(cases, "R9a"), .engine = Engine::kOurs};
  const auto tape = RunOurs(c, OurVariant::kScalar, kSmokeSeed, std::uint64_t{1} << 18, kBatches);
  ASSERT_TRUE(tape.has_value());
  EXPECT_LT(std::fabs(tape->mean - r9.asian_delta), 5.0 * tape->standard_error + r9.asian_b)
      << tape->mean << " +- " << tape->standard_error << " against " << r9.asian_delta;
}

TEST(QlCompareOurs, ControlVariateFindsTheReferenceWithLessNoise) {
  const std::vector<Case> cases = AccuracyCases();
  const Case r3 = CaseById(cases, "R3");
  const std::uint64_t paths = std::uint64_t{1} << 16;
  const auto cv = RunOurs({.contract = r3, .engine = Engine::kOurs}, OurVariant::kScalar,
                          kSmokeSeed, paths, kBatches);
  const auto plain = pricer::Price(OurContract(r3, Engine::kOurs), OurMarket(r3),
                                   {.seed = kSmokeSeed, .paths = paths, .batches = kBatches});
  ASSERT_TRUE(cv.has_value()) << cv.error();
  ASSERT_TRUE(plain.has_value());
  EXPECT_LT(std::fabs(cv->mean - 4.881961616712), 4.5 * cv->standard_error);
  EXPECT_LT(cv->standard_error, 0.1 * plain->standard_error);
  EXPECT_FALSE(RunOurs({.contract = r3, .engine = Engine::kOurs}, OurVariant::kLanes, kSmokeSeed,
                       paths, kBatches)
                   .has_value());
}

TEST(QlCompareOurs, LanesGiveTheScalarBits) {
  const std::uint64_t paths = std::uint64_t{1} << 12;
  for (const Comparison& c : Comparisons()) {
    if (c.engine == Engine::kQuantLib || !HasLaneForm(c)) continue;
    const auto scalar = RunOurs(c, OurVariant::kScalar, kSmokeSeed, paths, kBatches);
    const auto lanes = RunOurs(c, OurVariant::kLanes, kSmokeSeed, paths, kBatches);
    ASSERT_TRUE(scalar.has_value() && lanes.has_value()) << c.Name();
    EXPECT_EQ(std::bit_cast<std::uint64_t>(scalar->mean), std::bit_cast<std::uint64_t>(lanes->mean))
        << c.Name();
    EXPECT_EQ(scalar->batch_means, lanes->batch_means) << c.Name();
  }
}

LocalVolGrid SmallGrid() {
  LocalVolGrid g;
  g.times = {0.0, 0.5, 1.0};
  g.spots = {80.0, 100.0, 120.0};
  // vols[time * 3 + spot]
  g.vols = {0.30, 0.20, 0.10, 0.40, 0.30, 0.20, 0.50, 0.40, 0.30};
  return g;
}

TEST(QlCompareGrid, FollowsFixedLocalVolSurfacesRule) {
  const LocalVolGrid g = SmallGrid();
  EXPECT_EQ(g.Vol(0.5, 100.0), 0.30);
  EXPECT_DOUBLE_EQ(g.Vol(0.5, 90.0), 0.35);
  // On a grid time: linear extrapolation in spot.
  EXPECT_DOUBLE_EQ(g.Vol(0.5, 140.0), 0.10);
  EXPECT_DOUBLE_EQ(g.Vol(0.0, 60.0), 0.40);
  // Between times: the spot held inside the grid, then linear in volatility in time.
  EXPECT_DOUBLE_EQ(g.Vol(0.25, 140.0), 0.15);
  EXPECT_DOUBLE_EQ(g.Vol(0.75, 90.0), 0.40);
  // Times clamped to the grid.
  EXPECT_DOUBLE_EQ(g.Vol(2.0, 100.0), 0.40);
  EXPECT_DOUBLE_EQ(g.Vol(-1.0, 100.0), 0.20);
  EXPECT_TRUE(CloseEnough(0.3, 0.1 + 0.2));
  EXPECT_FALSE(CloseEnough(0.3, 0.3000001));
}

TEST(QlCompareGrid, BuildsFromTheSyntheticSurface) {
  const auto grid = BuildGrid(MakeSyntheticSurface());
  ASSERT_TRUE(grid.has_value());
  ASSERT_EQ(grid->times.size(), 51U);
  ASSERT_EQ(grid->spots.size(), 111U);
  EXPECT_EQ(grid->times.back(), 1.0);
  EXPECT_EQ(grid->spots.back(), 250.0);
  for (const double v : grid->vols) {
    EXPECT_TRUE(v >= 0.01 && v <= 2.0) << v;
  }
  // Near the money the local volatility is near the 20% implied.
  const double atm = grid->Vol(0.5, 105.0);
  EXPECT_GT(atm, 0.15);
  EXPECT_LT(atm, 0.25);
}

// A flat grid is GBM: the grid model must price as the production GBM model does.
TEST(QlCompareGrid, AFlatGridPricesAsGbm) {
  LocalVolGrid flat = SmallGrid();
  for (double& v : flat.vols) v = 0.25;
  const pricer::RunConfig config{.seed = kSmokeSeed, .paths = 1U << 14, .batches = kBatches};
  for (const LocalVolRow row : {LocalVolRow::kR10, LocalVolRow::kR11}) {
    const auto on_grid = PriceOnGrid(row, flat, config);
    pricer::Market<double> market = LocalVolRowMarket(row);
    market.sigma = 0.25;
    const auto gbm = pricer::Price(LocalVolRowContract(row), market, config);
    ASSERT_TRUE(on_grid.has_value() && gbm.has_value());
    EXPECT_NEAR(on_grid->mean, gbm->mean, 1e-9 * std::fabs(gbm->mean));
  }
}

TEST(QlCompareGrid, R12ReferenceIsBlackAtTheSliceVolatility) {
  const SyntheticSurface s = MakeSyntheticSurface();
  const double t = s.terms.back().expiry;
  const double k = std::log(kR12Strike / s.terms.back().forward);
  const double w = svi::TotalVariance(s.params.back(), k);
  const MarketParams<double> m{
      .spot = s.spot, .sigma = std::sqrt(w / t), .r = s.r, .q = s.q, .maturity = t};
  EXPECT_NEAR(R12Reference(s), BlackScholes(OptionType::kCall, kR12Strike, m).value, 1e-10);
}

TEST(QlCompareCeilings, RuleAndFile) {
  EXPECT_DOUBLE_EQ(RoundUpThreeFigures(1.2341e-3), 1.24e-3);
  EXPECT_DOUBLE_EQ(RoundUpThreeFigures(5.0), 5.0);
  EXPECT_DOUBLE_EQ(CeilingFrom(0.01, std::uint64_t{1} << 20, std::uint64_t{1} << 22, 1.65),
                   0.00825);
  EXPECT_DOUBLE_EQ(CeilingFrom(0.01, std::uint64_t{1} << 20, std::uint64_t{1} << 22, 3.0), 0.015);

  std::vector<CeilingRow> rows;
  double se = 1e-3;
  for (const Comparison& c : Comparisons()) {
    rows.push_back({.name = c.Name(),
                    .se_pilot = se,
                    .n_pilot = kPilotPaths,
                    .factor = CeilingFactor(c),
                    .ceiling = CeilingFrom(se, kPilotPaths, kAccuracyPaths, CeilingFactor(c))});
    se *= 1.1;
  }
  const std::string text = RenderCeilings(rows, "line one\nline two");
  const auto decoded = DecodeCeilings(text);
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  ASSERT_EQ(decoded->size(), rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ((*decoded)[i].name, rows[i].name);
    EXPECT_EQ((*decoded)[i].ceiling, rows[i].ceiling);
    EXPECT_EQ((*decoded)[i].se_pilot, rows[i].se_pilot);
  }
  rows.pop_back();
  EXPECT_FALSE(DecodeCeilings(RenderCeilings(rows, "short")).has_value());
  rows.push_back(rows.front());
  EXPECT_FALSE(DecodeCeilings(RenderCeilings(rows, "duplicate")).has_value());
  for (const Comparison& c : Comparisons()) {
    EXPECT_EQ(CeilingFactor(c), IsQuantLibBump(c) ? 3.0 : 1.65) << c.Name();
  }
}

TEST(QlCompareMetrics, PathsStepsAndWorkNormalisedError) {
  const Timing t{.median_s = 2.0, .min_s = 1.0, .max_s = 3.0, .paths = 1000, .steps = 50};
  EXPECT_DOUBLE_EQ(PathsPerSecond(t), 500.0);
  EXPECT_DOUBLE_EQ(NanosecondsPerPathStep(t), 2.0e9 / 50000.0);
  EXPECT_DOUBLE_EQ(MeanSquaredError(0.1, 0.05), 0.01);
  EXPECT_DOUBLE_EQ(MeanSquaredError(0.1, 0.3), 0.09);
  // 0.01 squared error at 4000 paths, 2 ms per path: 0.01 * 8 s.
  EXPECT_DOUBLE_EQ(ErrorTimesTime(0.01, t, 4000), 0.08);
}

}  // namespace
}  // namespace aadp::qlcompare
