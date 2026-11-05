// The local-volatility pricer (pricer/local_vol.hpp) against NOTES.md, "Stage 7a", whose
// pre-registration states every tolerance here before this test existed.
//
// 1. The sub-stepped kernels at n = 1 on the GBM model give EvaluatePath's bits, for every product
//    and treatment they accept. No tolerance: the operations are the same.
// 2. At n = 4, a loop written out here gives their bits for the Asian and the discrete hard
//    barrier, so the fixing and monitoring stride is checked by something other than the kernel.
// 3. The flat-surface oracle: b = 0 and a_j = sigma^2 T_j on every slice, so sigma_loc = sigma, no
//    floor and no cap. On the same uniforms the local-volatility model equals the GBM model in the
//    same kernel, path by path, within 2^-30 D(T) max(S0, K): log-Euler with a constant volatility
//    is exact GBM, and the two differ only by the roundings of their drift, variance and
//    discount, about 1e-11 of that scale at most (NOTES.md gives the derivation).
// 4. Validation names each refusal; a maturity one second past the last slice is refused.
// 5. Price, SE, batch means and the floor tally are the same bits for 1, 2, 3 and 8 threads, and
//    the tally adds up.
#include "pricer/local_vol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/model.hpp"
#include "pricer/real_ops.hpp"
#include "pricer/substep_kernels.hpp"
#include "svi/local_vol.hpp"
#include "svi/surface.hpp"
#include "svi/synthetic_surface.hpp"

namespace aadp::pricer {
namespace {

using svi::testing::Surface;

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261116;
constexpr std::uint32_t kDates = 11;
constexpr double kStrike = 100.0;

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

struct Case {
  std::string name;
  Contract contract;
};

// European or arithmetic Asian call, struck at 100, on kDates dates.
Contract Plain(Product product) {
  Contract c;
  c.product = product;
  c.strike = 100.0;
  c.steps = kDates;
  return c;
}

Contract Barrier(Direction d, Monitoring m, Treatment t, double level, OptionType type) {
  return {.product = Product::kBarrier,
          .type = type,
          .strike = kStrike,
          .steps = kDates,
          .barrier = {.direction = d,
                      .knock = Knock::kOut,
                      .monitoring = m,
                      .treatment = t,
                      .level = level,
                      .smoothing_width = t == Treatment::kSmoothed ? 0.005 : 0.0}};
}

std::vector<Case> Cases() {
  const OptionType call = OptionType::kCall;
  return {
      {.name = "european", .contract = Plain(Product::kEuropean)},
      {.name = "asian", .contract = Plain(Product::kAsianArithmetic)},
      {.name = "doc-discrete-one-step",
       .contract =
           Barrier(Direction::kDown, Monitoring::kDiscrete, Treatment::kOneStep, 95.0, call)},
      {.name = "doc-discrete-hard",
       .contract = Barrier(Direction::kDown, Monitoring::kDiscrete, Treatment::kHard, 95.0, call)},
      {.name = "doc-discrete-smoothed",
       .contract =
           Barrier(Direction::kDown, Monitoring::kDiscrete, Treatment::kSmoothed, 95.0, call)},
      {.name = "doc-continuous-bridge",
       .contract =
           Barrier(Direction::kDown, Monitoring::kContinuous, Treatment::kBridge, 95.0, call)},
      {.name = "doc-continuous-sampled",
       .contract =
           Barrier(Direction::kDown, Monitoring::kContinuous, Treatment::kHard, 95.0, call)},
      {.name = "uop-continuous-bridge",
       .contract = Barrier(Direction::kUp, Monitoring::kContinuous, Treatment::kBridge, 105.0,
                           OptionType::kPut)},
      {.name = "uop-discrete-one-step",
       .contract = Barrier(Direction::kUp, Monitoring::kDiscrete, Treatment::kOneStep, 105.0,
                           OptionType::kPut)},
  };
}

LocalVolContract OnSurface(const Contract& c, std::uint32_t n) {
  return {.contract = c, .maturity = svi::testing::kExpiries[2], .substeps = n};
}

Market<double> FlatMarket(double spot) {
  return {.spot = spot,
          .sigma = svi::testing::kFlatVol,
          .r = svi::testing::kFlatRate,
          .q = 0.0,
          .maturity = svi::testing::kExpiries[2]};
}

struct Draws {
  std::vector<double> normal;
  std::vector<double> crossing;
  explicit Draws(std::uint32_t steps) : normal(steps), crossing(steps) {}
  PathUniforms Fill(std::uint64_t path) {
    FillPath(kSeed, path, normal, crossing);
    return {.normal = normal, .crossing = crossing};
  }
};

TEST(SubstepKernels, OneSubStepIsEvaluatePathBitForBit) {
  const Market<double> market = FlatMarket(100.0);
  const GbmModel<double> model(market, kDates);
  Draws draws(kDates);
  for (const Case& c : Cases()) {
    ASSERT_EQ(Validate(c.contract, market), ContractError::kNone) << c.name;
    ASSERT_TRUE(SubstepSupports(c.contract)) << c.name;
    for (std::uint64_t path = 0; path < 2048; ++path) {
      const PathUniforms u = draws.Fill(path);
      const PathValue<double> want = EvaluatePath<double>(c.contract, model, u);
      const PathValue<double> got = EvaluateSubstepped<double>(c.contract, 1, model, u);
      ASSERT_EQ(Bits(got.value), Bits(want.value)) << c.name << ", path " << path;
      ASSERT_EQ(Bits(got.weight), Bits(want.weight)) << c.name << ", path " << path;
    }
  }
}

TEST(SubstepKernels, AsiansFixAndDiscreteBarriersMonitorOnlyAtTheDates) {
  constexpr std::uint32_t kN = 4;
  const Market<double> market = FlatMarket(100.0);
  const GbmModel<double> model(market, kDates * kN);
  const Contract asian = Plain(Product::kAsianArithmetic);
  const Contract hard =
      Barrier(Direction::kDown, Monitoring::kDiscrete, Treatment::kHard, 97.0, OptionType::kCall);
  const double log_barrier = Log(97.0);
  Draws draws(kDates * kN);
  std::size_t killed = 0;
  for (std::uint64_t path = 0; path < 4096; ++path) {
    const PathUniforms u = draws.Fill(path);
    double x = model.LogSpot();
    double sum = 0.0;
    double weight = 1.0;
    for (std::uint32_t s = 0; s < kDates * kN; ++s) {
      const StepLaw<double> law = model.Law(s, x);
      x = law.mean + law.sd * InverseNormal(u.normal[s]);
      if (s % kN == kN - 1) {
        sum = sum + Exp(x);
        weight = weight * (x > log_barrier ? 1.0 : 0.0);
      }
    }
    const double average = sum / static_cast<double>(kDates) - kStrike;
    const double want_asian = model.Discount() * (average > 0.0 ? average : 0.0);
    const double terminal = Exp(x) - kStrike;
    const double want_hard = model.Discount() * weight * (terminal > 0.0 ? terminal : 0.0);
    killed += weight == 0.0 ? 1 : 0;
    ASSERT_EQ(Bits(EvaluateSubstepped<double>(asian, kN, model, u).value), Bits(want_asian))
        << "path " << path;
    ASSERT_EQ(Bits(EvaluateSubstepped<double>(hard, kN, model, u).value), Bits(want_hard))
        << "path " << path;
  }
  EXPECT_GT(killed, 100U);
}

TEST(LocalVol, AFlatSurfaceIsGbmPathByPathWithinTheRoundingBound) {
  const Surface flat = svi::testing::FlatSurface();
  const svi::SviSurface<double> surface(flat.terms, flat.params);
  const double spot = surface.Curve().Spot();
  const Market<double> market = FlatMarket(spot);
  for (const std::uint32_t n : {1U, 4U}) {
    const std::uint32_t steps = kDates * n;
    LocalVolModel<double> lv(surface, svi::testing::kExpiries[2], steps, {});
    FloorTally tally;
    lv.SetTally(&tally);
    const GbmModel<double> gbm(market, steps);
    const double bound = 0x1p-30 * lv.Discount() * std::max(spot, kStrike);
    Draws draws(steps);
    for (const Case& c : Cases()) {
      ASSERT_EQ(ValidateLocalVol(OnSurface(c.contract, n), flat.terms, flat.params, {}).code,
                LocalVolError::kNone)
          << c.name;
      double worst = 0.0;
      for (std::uint64_t path = 0; path < 4096; ++path) {
        const PathUniforms u = draws.Fill(path);
        const double want = EvaluateSubstepped<double>(c.contract, n, gbm, u).value;
        const double got = EvaluateSubstepped<double>(c.contract, n, lv, u).value;
        ASSERT_LE(std::fabs(got - want), bound) << c.name << ", n " << n << ", path " << path
                                                << ": local vol " << got << ", GBM " << want;
        worst = std::max(worst, std::fabs(got - want));
      }
      std::printf("measured, flat surface, %s, n %u: max |LV - GBM| / bound %.3g\n", c.name.c_str(),
                  n, worst / bound);
    }
    EXPECT_EQ(tally.Floored(), 0U);
    EXPECT_EQ(tally.Capped(), 0U);
    EXPECT_EQ(tally.by_class[static_cast<std::size_t>(svi::LocalVarianceClass::kOk)],
              tally.path_steps);
  }
}

TEST(LocalVol, ValidationNamesEachRefusal) {
  const Surface s = svi::testing::SurfaceB();
  const Contract asian = Plain(Product::kAsianArithmetic);
  const auto code = [&](const LocalVolContract& c, const svi::LocalVolRule& rule = {}) {
    return ValidateLocalVol(c, s.terms, s.params, rule).code;
  };
  EXPECT_EQ(code(OnSurface(asian, 4)), LocalVolError::kNone);
  // The last slice's own expiry is the longest maturity; one second past it is refused.
  LocalVolContract past = OnSurface(asian, 4);
  past.maturity = svi::testing::kExpiries[2] + (1.0 / (365.0 * 86400.0));
  EXPECT_EQ(code(past), LocalVolError::kMaturity);
  EXPECT_EQ(code(OnSurface(asian, 0)), LocalVolError::kSubsteps);
  EXPECT_EQ(code(OnSurface(asian, 20000)), LocalVolError::kSubsteps);
  Contract digital = asian;
  digital.product = Product::kDigital;
  EXPECT_EQ(code(OnSurface(digital, 4)), LocalVolError::kProduct);
  Contract with_spot = asian;
  with_spot.include_spot = true;
  EXPECT_EQ(code(OnSurface(with_spot, 4)), LocalVolError::kProduct);
  Contract knock_in = Barrier(Direction::kDown, Monitoring::kContinuous, Treatment::kBridge, 95.0,
                              OptionType::kCall);
  knock_in.barrier.knock = Knock::kIn;
  EXPECT_EQ(code(OnSurface(knock_in, 4)), LocalVolError::kProduct);
  const Contract on_discrete =
      Barrier(Direction::kDown, Monitoring::kDiscrete, Treatment::kBridge, 95.0, OptionType::kCall);
  EXPECT_EQ(code(OnSurface(on_discrete, 4)), LocalVolError::kContract);
  EXPECT_EQ(code(OnSurface(asian, 4), {.floor = 0.0, .cap = 4.0}), LocalVolError::kRule);
  EXPECT_EQ(code(OnSurface(asian, 4), {.floor = 1e-4, .cap = 1e-4}), LocalVolError::kRule);
  EXPECT_EQ(code(OnSurface(asian, 4), {.floor = 1e-4, .cap = 101.0}), LocalVolError::kRule);
  // At a cap of 100 (volatility 10) and 10,000 steps, 9 sd per step could reach exp(340).
  EXPECT_EQ(code(OnSurface(asian, 900), {.floor = 1e-4, .cap = 100.0}), LocalVolError::kRange);
  Surface bad = s;
  bad.params[1].b = -1.0;
  EXPECT_EQ(ValidateLocalVol(OnSurface(asian, 4), bad.terms, bad.params, {}).code,
            LocalVolError::kSurface);
  EXPECT_EQ(PriceLocalVol(OnSurface(asian, 4), s.terms, s.params,
                          {.seed = kSeed, .paths = 10, .batches = 1, .threads = 1})
                .error()
                .code,
            LocalVolError::kRun);
}

void ExpectSameTally(const FloorTally& a, const FloorTally& b, const std::string& where) {
  EXPECT_EQ(a.paths, b.paths) << where;
  EXPECT_EQ(a.path_steps, b.path_steps) << where;
  EXPECT_EQ(a.paths_with_floor, b.paths_with_floor) << where;
  EXPECT_EQ(a.paths_with_cap, b.paths_with_cap) << where;
  EXPECT_EQ(a.by_class, b.by_class) << where;
  for (std::size_t i = 0; i < a.floored.size(); ++i) {
    for (const auto& [x, y] :
         {std::pair{a.floored[i], b.floored[i]}, std::pair{a.capped[i], b.capped[i]}}) {
      EXPECT_EQ(x.count, y.count) << where;
      EXPECT_EQ(Bits(x.k_min), Bits(y.k_min)) << where;
      EXPECT_EQ(Bits(x.k_max), Bits(y.k_max)) << where;
      EXPECT_EQ(Bits(x.t_min), Bits(y.t_min)) << where;
      EXPECT_EQ(Bits(x.t_max), Bits(y.t_max)) << where;
    }
  }
}

TEST(LocalVol, TheSameBitsAndTalliesForAnyThreadCountAndTheTallyAddsUp) {
  const Surface s = svi::testing::SurfaceB();
  constexpr std::uint64_t kPaths = 8192;
  for (const Case& c : Cases()) {
    const LocalVolContract lv = OnSurface(c.contract, 4);
    const auto one = PriceLocalVol(lv, s.terms, s.params,
                                   {.seed = kSeed, .paths = kPaths, .batches = 8, .threads = 1});
    ASSERT_TRUE(one.has_value()) << c.name;
    const FloorTally& t = one->floors;
    std::uint64_t classes = 0;
    std::uint64_t floored = 0;
    for (std::size_t i = 0; i < t.by_class.size(); ++i) {
      classes += t.by_class[i];
      if (svi::IsFloored(static_cast<svi::LocalVarianceClass>(i))) floored += t.by_class[i];
    }
    EXPECT_EQ(t.paths, kPaths) << c.name;
    EXPECT_EQ(t.path_steps, kPaths * kDates * 4) << c.name;
    EXPECT_EQ(classes, t.path_steps) << c.name;
    EXPECT_EQ(t.Floored(), floored) << c.name;
    EXPECT_EQ(t.Capped(), t.by_class[static_cast<std::size_t>(svi::LocalVarianceClass::kCapped)]);
    EXPECT_LE(t.paths_with_floor, t.paths) << c.name;
    EXPECT_GT(t.Floored(), 0U) << c.name << ": surface B is built to floor";
    EXPECT_GT(t.paths_with_floor, 0U) << c.name;
    for (const std::uint32_t threads : {2U, 3U, 8U}) {
      const auto many =
          PriceLocalVol(lv, s.terms, s.params,
                        {.seed = kSeed, .paths = kPaths, .batches = 8, .threads = threads});
      ASSERT_TRUE(many.has_value());
      const std::string where = c.name + ", " + std::to_string(threads) + " threads";
      EXPECT_EQ(Bits(many->value.mean), Bits(one->value.mean)) << where;
      EXPECT_EQ(Bits(many->value.standard_error), Bits(one->value.standard_error)) << where;
      for (std::size_t b = 0; b < one->value.batch_means.size(); ++b) {
        EXPECT_EQ(Bits(many->value.batch_means[b]), Bits(one->value.batch_means[b])) << where;
      }
      ExpectSameTally(many->floors, t, where);
    }
    std::printf(
        "measured, surface B, %s, n 4, %llu paths: price %.6g, floored %llu of %llu path-steps, "
        "capped %llu, paths with a floor %llu\n",
        c.name.c_str(), static_cast<unsigned long long>(kPaths), one->value.mean,
        static_cast<unsigned long long>(t.Floored()), static_cast<unsigned long long>(t.path_steps),
        static_cast<unsigned long long>(t.Capped()),
        static_cast<unsigned long long>(t.paths_with_floor));
  }
}

}  // namespace
}  // namespace aadp::pricer
