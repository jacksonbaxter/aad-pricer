// Check 3: pricer estimates against closed forms and committed references, at a development seed,
// with the repo's tolerance rule (src/oracle/tolerance.hpp). Never seeds 1729 or 271828.
//
// The family, stated before it ran: the kFamily comparisons listed in Rows() below, so K = kFamily
// in the Bonferroni critical value t(1 - alpha_F / (2K), B - 1), alpha_F = 0.01, B = 64. b is the
// reference's own allowance: 0 for a closed form, the oracle_values.csv b for a recursion. The SE
// ceiling of every row is its scale (discount times the larger of S0 and K, or the discount for a
// digital) over sqrt(N): the SE of a payoff whose standard deviation were its whole scale. Every
// product here has a smaller one, so a larger SE means a broken estimator, not bad luck.
//
// Not here: the smoothed barrier, which is biased by design and measured by tools/pricer_bias.
// The knock-in parity identity is exact, per path, below.
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "oracle/barrier.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/geometric_asian.hpp"
#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "rng/normal.hpp"

#ifndef AADP_SMOKE_SIZE
#error "cmake/area_pricer.cmake sets AADP_SMOKE_SIZE"
#endif

namespace aadp::pricer {
namespace {

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261108;
constexpr std::uint32_t kBatches = 64;
constexpr std::uint64_t kPaths =
    AADP_SMOKE_SIZE ? (std::uint64_t{1} << 14) : (std::uint64_t{1} << 20);
constexpr std::size_t kFamily = 28;

using aadp::BarrierType;

struct Row {
  std::string name;
  Contract contract;
  Market<double> market;
  double reference = 0.0;
  double b = 0.0;
  // Set for a paired row: the reference is this contract's estimate on the same paths.
  std::optional<Contract> paired;
};

const ReferenceTable& References() {
  static const ReferenceTable table = [] {
    std::ifstream in("references/oracle_values.csv");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto decoded = DecodeReferences(text);
    return decoded ? *decoded : ReferenceTable{};
  }();
  return table;
}

double ReferenceValue(const char* id) {
  const Reference* r = References().Find(id);
  return r == nullptr ? std::nan("") : r->value;
}
double ReferenceB(const char* id) {
  const Reference* r = References().Find(id);
  return r == nullptr ? std::nan("") : r->b;
}

aadp::MarketParams<double> Oracle(const Market<double>& m) {
  return {.spot = m.spot, .sigma = m.sigma, .r = m.r, .q = m.q, .maturity = m.maturity};
}
aadp::OptionType Oracle(OptionType t) {
  return t == OptionType::kCall ? aadp::OptionType::kCall : aadp::OptionType::kPut;
}

Contract Plain(Product product, OptionType type, double strike, std::uint32_t steps,
               bool include_spot = false) {
  Contract c;
  c.product = product;
  c.type = type;
  c.strike = strike;
  c.steps = steps;
  c.include_spot = include_spot;
  return c;
}

Contract WithBarrier(OptionType type, double strike, std::uint32_t steps, Direction direction,
                     Knock knock, Monitoring monitoring, Treatment treatment, double level) {
  Contract c = Plain(Product::kBarrier, type, strike, steps);
  c.barrier = {.direction = direction,
               .knock = knock,
               .monitoring = monitoring,
               .treatment = treatment,
               .level = level,
               .smoothing_width = 0.0};
  return c;
}

double BarrierValue(const Contract& c, const Market<double>& m) {
  const bool down = c.barrier.direction == Direction::kDown;
  const bool out = c.barrier.knock == Knock::kOut;
  const BarrierType down_type = out ? BarrierType::kDownOut : BarrierType::kDownIn;
  const BarrierType up_type = out ? BarrierType::kUpOut : BarrierType::kUpIn;
  const BarrierType type = down ? down_type : up_type;
  return BarrierOptionValue(type, Oracle(c.type), c.strike, c.barrier.level, 0.0, Oracle(m));
}

double Digital(OptionType type, double strike, const Market<double>& m) {
  const double d2 =
      (std::log(m.spot / strike) + ((m.r - m.q - (0.5 * m.sigma * m.sigma)) * m.maturity)) /
      (m.sigma * std::sqrt(m.maturity));
  return std::exp(-m.r * m.maturity) * aadp::normal_cdf(type == OptionType::kCall ? d2 : -d2);
}

std::vector<Row> Rows() {
  using enum OptionType;
  using enum Direction;
  using enum Knock;
  using enum Monitoring;
  using enum Treatment;
  const Market<double> va{.spot = 105.0, .sigma = 0.36, .r = 0.1, .q = 0.1, .maturity = 0.5};
  const Market<double> vc{
      .spot = 430.0, .sigma = 0.2, .r = 0.07, .q = 0.05, .maturity = 1.0 / 12.0};
  const Market<double> ga{.spot = 100.0, .sigma = 0.2, .r = 0.06, .q = 0.03, .maturity = 1.0};
  const Market<double> aa{.spot = 100.0, .sigma = 0.17801, .r = 0.0367, .q = 0.0, .maturity = 1};
  const Market<double> db{.spot = 100.0, .sigma = 0.3, .r = 0.1, .q = 0.0, .maturity = 0.2};
  const Market<double> dbs{.spot = 100.0, .sigma = 0.6, .r = 0.1, .q = 0.0, .maturity = 0.2};
  const Market<double> cbk{.spot = 100.0, .sigma = 0.25, .r = 0.08, .q = 0.04, .maturity = 0.5};

  std::vector<Row> rows;
  const auto closed = [&](std::string name, const Contract& c, const Market<double>& m,
                          double value) {
    rows.push_back({.name = std::move(name),
                    .contract = c,
                    .market = m,
                    .reference = value,
                    .b = 0.0,
                    .paired = std::nullopt});
  };
  const auto referenced = [&](std::string name, const Contract& c, const Market<double>& m,
                              const char* id) {
    rows.push_back({.name = std::move(name),
                    .contract = c,
                    .market = m,
                    .reference = ReferenceValue(id),
                    .b = ReferenceB(id),
                    .paired = std::nullopt});
  };

  closed("BS call", Plain(Product::kEuropean, kCall, 100, 1), va,
         BlackScholes(aadp::OptionType::kCall, 100, Oracle(va)).value);
  closed("BS put", Plain(Product::kEuropean, kPut, 405, 1), vc,
         BlackScholes(aadp::OptionType::kPut, 405, Oracle(vc)).value);
  closed("digital call", Plain(Product::kDigital, kCall, 105, 5), ga, Digital(kCall, 105, ga));
  closed("digital put", Plain(Product::kDigital, kPut, 95, 5), ga, Digital(kPut, 95, ga));
  closed("geometric Asian call", Plain(Product::kAsianGeometric, kCall, 100, 10), ga,
         GeometricAsianValue(aadp::OptionType::kCall, 100, 10, false, Oracle(ga)));
  closed("geometric Asian put with spot", Plain(Product::kAsianGeometric, kPut, 100, 12, true), ga,
         GeometricAsianValue(aadp::OptionType::kPut, 100, 12, true, Oracle(ga)));
  referenced("arithmetic Asian AA12K100", Plain(Product::kAsianArithmetic, kCall, 100, 12, true),
             aa, "AA12K100");
  referenced("arithmetic Asian AA50K090", Plain(Product::kAsianArithmetic, kCall, 90, 50, true), aa,
             "AA50K090");

  const auto bridge = [&](std::string name, OptionType type, double strike, std::uint32_t steps,
                          Direction direction, Knock knock, Treatment treatment, double level,
                          const Market<double>& m) {
    const Contract c =
        WithBarrier(type, strike, steps, direction, knock, kContinuous, treatment, level);
    closed(std::move(name), c, m, BarrierValue(c, m));
  };
  bridge("bridge down-out call H95", kCall, 100, 50, kDown, kOut, kBridge, 95, db);
  bridge("bridge down-out call K90 H95", kCall, 90, 25, kDown, kOut, kBridge, 95, cbk);
  bridge("bridge down-out put H90", kPut, 100, 50, kDown, kOut, kBridge, 90, db);
  bridge("bridge up-out call H110", kCall, 100, 50, kUp, kOut, kBridge, 110, db);
  bridge("bridge up-out put H110", kPut, 100, 50, kUp, kOut, kBridge, 110, db);
  bridge("bridge down-in call H95", kCall, 100, 50, kDown, kIn, kBridge, 95, db);
  bridge("bridge up-in put H110", kPut, 100, 50, kUp, kIn, kBridge, 110, db);
  bridge("sampled crossing down-out call H95", kCall, 100, 50, kDown, kOut, kHard, 95, db);
  bridge("sampled crossing up-out put H110", kPut, 100, 50, kUp, kOut, kHard, 110, db);

  const auto discrete = [&](OptionType type, Direction direction, Knock knock, Treatment treatment,
                            double level, std::uint32_t steps) {
    return WithBarrier(type, 100, steps, direction, knock, kDiscrete, treatment, level);
  };
  referenced("one-step DB085", discrete(kCall, kDown, kOut, kOneStep, 85, 50), db, "DB085.value");
  referenced("one-step DB090", discrete(kCall, kDown, kOut, kOneStep, 90, 50), db, "DB090.value");
  referenced("one-step DB095", discrete(kCall, kDown, kOut, kOneStep, 95, 50), db, "O5");
  referenced("one-step DB097", discrete(kCall, kDown, kOut, kOneStep, 97, 50), db, "DB097.value");
  referenced("one-step DB099", discrete(kCall, kDown, kOut, kOneStep, 99, 50), db, "DB099.value");
  referenced("one-step DBS", discrete(kCall, kDown, kOut, kOneStep, 95, 4), dbs, "O8");
  referenced("hard DB095", discrete(kCall, kDown, kOut, kHard, 95, 50), db, "O5");

  // Knock-in: vanilla less the reference knock-out. Its b is the knock-out's.
  const double vanilla = BlackScholes(aadp::OptionType::kCall, 100, Oracle(db)).value;
  for (const Treatment treatment : {kOneStep, kHard}) {
    rows.push_back({.name = treatment == kOneStep ? "one-step down-in DB095" : "hard down-in DB095",
                    .contract = discrete(kCall, kDown, kIn, treatment, 95, 50),
                    .market = db,
                    .reference = vanilla - ReferenceValue("O5"),
                    .b = ReferenceB("O5"),
                    .paired = std::nullopt});
  }
  // Up barriers have no committed discrete reference: one-step against hard on the same paths,
  // two unbiased estimators of the same contract.
  for (const OptionType type : {kCall, kPut}) {
    rows.push_back({.name = type == kCall ? "one-step up-out call against hard, paired"
                                          : "one-step up-out put against hard, paired",
                    .contract = discrete(type, kUp, kOut, kOneStep, 110, 50),
                    .market = db,
                    .paired = discrete(type, kUp, kOut, kHard, 110, 50)});
  }
  return rows;
}

double Scale(const Row& row) {
  const double discount = std::exp(-row.market.r * row.market.maturity);
  if (row.contract.product == Product::kDigital) return discount;
  return discount * std::max(row.market.spot, row.contract.strike);
}

std::vector<double> BatchMeans(const Contract& c, const Market<double>& m) {
  const auto estimate =
      Price(c, m, {.seed = kSeed, .paths = kPaths, .batches = kBatches, .threads = 8});
  EXPECT_TRUE(estimate.has_value());
  return estimate ? estimate->batch_means : std::vector<double>{};
}

TEST(PricerClosedForm, TheFamilyIsAsStated) {
  ASSERT_FALSE(References().rows.empty()) << "references/oracle_values.csv";
  const std::vector<Row> rows = Rows();
  EXPECT_EQ(rows.size(), kFamily);
  for (const Row& row : rows) {
    EXPECT_EQ(Validate(row.contract, row.market), ContractError::kNone) << row.name;
    if (!row.paired) {
      EXPECT_TRUE(std::isfinite(row.reference)) << row.name;
    }
  }
}

TEST(PricerClosedForm, EveryRowPassesTheToleranceRule) {
  const double critical = CriticalValue(kFamilyWiseAlpha, kFamily, kBatches);
  for (const Row& row : Rows()) {
    const ToleranceLimits limits{.b = row.b,
                                 .se_ceiling = Scale(row) / std::sqrt(static_cast<double>(kPaths)),
                                 .critical = critical};
    const std::vector<double> means = BatchMeans(row.contract, row.market);
    const ToleranceResult result =
        row.paired ? CheckPaired(means, BatchMeans(*row.paired, row.market), limits)
                   : CheckAgainstReference(means, row.reference, limits);
    std::printf("%s: estimate %.6f reference %.6f SE %.2g bound %.2g ceiling %.2g, %s%s\n",
                row.name.c_str(), result.estimate, result.reference, result.standard_error,
                result.bound, result.se_ceiling, result.passed() ? "pass" : "FAIL",
                AADP_SMOKE_SIZE ? " (reduced size, not a result)" : "");
    EXPECT_TRUE(result.passed()) << row.name << ": " << result.failed_condition();
  }
}

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

// in = vanilla - out on every path, exactly, for every treatment and both directions.
TEST(PricerParity, KnockInIsVanillaLessKnockOutOnEveryPath) {
  using enum Treatment;
  const Market<double> m{.spot = 100.0, .sigma = 0.3, .r = 0.1, .q = 0.0, .maturity = 0.2};
  const GbmModel<double> model(m, 50);
  for (const Direction direction : {Direction::kDown, Direction::kUp}) {
    for (const OptionType type : {OptionType::kCall, OptionType::kPut}) {
      for (const auto& [monitoring, treatment] :
           {std::pair{Monitoring::kContinuous, kBridge}, std::pair{Monitoring::kContinuous, kHard},
            std::pair{Monitoring::kDiscrete, kOneStep}, std::pair{Monitoring::kDiscrete, kHard},
            std::pair{Monitoring::kDiscrete, kSmoothed}}) {
        const double level = direction == Direction::kDown ? 95.0 : 105.0;
        Contract in =
            WithBarrier(type, 100, 50, direction, Knock::kIn, monitoring, treatment, level);
        in.barrier.smoothing_width = 0.01;
        Contract out = in;
        out.barrier.knock = Knock::kOut;
        const Contract vanilla = Plain(Product::kEuropean, type, 100, 50);
        std::vector<double> normal(50);
        std::vector<double> crossing(50);
        const PathUniforms u{.normal = normal, .crossing = crossing};
        int knocked = 0;
        for (std::uint64_t path = 0; path < 4096; ++path) {
          FillPath(kSeed, path, normal, crossing);
          const auto v = DiscountedPayoff<double>(vanilla, model, u);
          const auto o = DiscountedPayoff<double>(out, model, u);
          const auto i = DiscountedPayoff<double>(in, model, u);
          ASSERT_EQ(Bits(i), Bits(v - o)) << static_cast<int>(treatment) << ", path " << path;
          knocked += o != v ? 1 : 0;
        }
        EXPECT_GT(knocked, 0) << static_cast<int>(treatment);
      }
    }
  }
}

}  // namespace
}  // namespace aadp::pricer
