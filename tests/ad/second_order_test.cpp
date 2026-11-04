// Per-path checks of the forward-over-reverse pass (src/ad/second_order.hpp), every bound stated
// before it ran. Development seed, never 1729 or 271828.
//
// Exactness. The pass's value is the double conditional kernel's, bit for bit; its gradient is
// the first-order tape's on the same kernel (ConditionalPathGradient), bit for bit, since its
// value lanes form the same products in the same order; and the spot row of a two-lane pass is
// the one-lane pass's, bit for bit, since lanes never mix.
//
// Check 3. Per path, row k of the Hessian against central differences of the first-order tape's
// own gradient, on the same uniforms (common random numbers), at steps h and h / 2 in input k:
//   |D(h/2) - H| <= |D(h) - D(h/2)| + R(h) + R(h/2) + E_H + 2^-50 (|D(h)| + |D(h/2)|).
// The truncation term: D(h) - H = c h^2 + O(h^4) for a path smooth in the input, so
// |D(h/2) - H| is about |D(h) - D(h/2)| / 3, and the term keeps a factor 3; where the error is
// first order (D(h/2) - H = c h / 2), it is |D(h) - D(h/2)| exactly. R(h) is the round-off of
// D(h): each gradient entry is a sum of products of partials, every partial a function of values
// computed with at most 8 roundings per statement, as the bump study counts them
// (bench/bump_study.hpp), and the sweep adds 2 T + O, so the noise in an entry is at most
// gamma_{10 T + O} times its absolute sum A, and R(h) = gamma_{10 T + O} (A+ + A-) / (x+ - x-).
// E_H is the pass's own round-off, HessianRoundoffBound. The term in 2^-50 covers the
// subtraction, the division and the comparison of the quotients, and F = 2^-1000 is an absolute
// floor for products below the normal range, where the relative model fails: each such product
// is wrong by at most 2^-1075, so F covers them through an amplification up to 2^75, and no Greek
// this pricer reports can see 2^-1000 (it averages at most 2^40 paths). Steps: spot 2^-12 S0,
// vol 2^-14, so the corner of one-step survival, where a path's gradient varies on a scale of
// about 1 in spot, is inside the h^2 regime.
//
// Check 4. Vanna two ways, per path: d^2 V / dS dvol from the spot lane at the vol input against
// d^2 V / dvol dS from the vol lane at the spot input. The exact forward over reverse of the
// recorded data is symmetric, because every elemental operation's matrix of second partials is
// (each binary operation's cross term is one number, 1 for a product and -1 / b^2 for a
// quotient), so the two differ only by round-off:
//   |H_Svol - H_volS| <= E_H(A_Svol) + E_H(A_volS) + F,
// E_H = HessianRoundoffBound, A the absolute pass's entry, F the floor of check 3. The paths with
// a nonzero partial or adjoint lane below 2^-511, where some product may be subnormal and only F
// covers it, are counted and printed.
#include "ad/second_order.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <string>
#include <vector>

#include "ad/bounds.hpp"
#include "ad/gradient.hpp"
#include "pricer/conditional_kernels.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/model.hpp"
#include "pricer/runner.hpp"

namespace aadp::ad {
namespace {

constexpr std::uint64_t kSeed = 20261113;
constexpr std::uint64_t kPaths = 256;
constexpr double kNormalFloor = 0x1p-511;
constexpr double kAbsoluteFloor = 0x1p-1000;

constexpr pricer::Market<double> kMarket{
    .spot = 100.0, .sigma = 0.30, .r = 0.10, .q = 0.02, .maturity = 0.5};
constexpr pricer::Market<double> kSecondMarket{
    .spot = 80.0, .sigma = 0.45, .r = -0.01, .q = 0.03, .maturity = 2.0};

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

struct Case {
  std::string name;
  pricer::Contract contract;
};

pricer::Contract Plain(pricer::Product product, pricer::OptionType type, double strike,
                       std::uint32_t steps, bool include_spot = false) {
  pricer::Contract c;
  c.product = product;
  c.type = type;
  c.strike = strike;
  c.steps = steps;
  c.include_spot = include_spot;
  return c;
}

pricer::Contract Barrier(pricer::OptionType type, double strike, std::uint32_t steps,
                         pricer::Direction direction, pricer::Knock knock,
                         pricer::Monitoring monitoring, double level) {
  pricer::Contract c = Plain(pricer::Product::kBarrier, type, strike, steps);
  c.barrier = {.direction = direction,
               .knock = knock,
               .monitoring = monitoring,
               .treatment = monitoring == pricer::Monitoring::kContinuous
                                ? pricer::Treatment::kBridge
                                : pricer::Treatment::kOneStep,
               .level = level,
               .smoothing_width = 0.0};
  return c;
}

// Every product, both option types, every direction and knock of both barrier estimators, with
// the strike on either side of the barrier. Strikes and levels are per 100 of spot.
std::vector<Case> Cases(std::uint32_t steps, double spot) {
  const auto Plain = [scale = spot / 100.0](pricer::Product product, pricer::OptionType type,
                                            double strike, std::uint32_t n,
                                            bool include_spot = false) {
    return ad::Plain(product, type, strike * scale, n, include_spot);
  };
  const auto Barrier = [scale = spot / 100.0](pricer::OptionType type, double strike,
                                              std::uint32_t n, pricer::Direction direction,
                                              pricer::Knock knock, pricer::Monitoring monitoring,
                                              double level) {
    return ad::Barrier(type, strike * scale, n, direction, knock, monitoring, level * scale);
  };
  using enum pricer::Product;
  using enum pricer::OptionType;
  using enum pricer::Direction;
  using enum pricer::Knock;
  using enum pricer::Monitoring;
  return {
      {.name = "european call", .contract = Plain(kEuropean, kCall, 105, steps)},
      {.name = "european put", .contract = Plain(kEuropean, kPut, 95, steps)},
      {.name = "digital call", .contract = Plain(kDigital, kCall, 100, steps)},
      {.name = "digital put", .contract = Plain(kDigital, kPut, 90, steps)},
      {.name = "arithmetic Asian call", .contract = Plain(kAsianArithmetic, kCall, 100, steps)},
      {.name = "arithmetic Asian put with spot",
       .contract = Plain(kAsianArithmetic, kPut, 100, steps, true)},
      {.name = "arithmetic Asian deep call with spot",
       .contract = Plain(kAsianArithmetic, kCall, 60, steps, true)},
      {.name = "geometric Asian call", .contract = Plain(kAsianGeometric, kCall, 100, steps)},
      {.name = "geometric Asian put with spot",
       .contract = Plain(kAsianGeometric, kPut, 100, steps, true)},
      {.name = "one-step down-out call",
       .contract = Barrier(kCall, 100, steps, kDown, kOut, kDiscrete, 90)},
      {.name = "one-step down-out call K below H",
       .contract = Barrier(kCall, 85, steps, kDown, kOut, kDiscrete, 90)},
      {.name = "one-step up-out put",
       .contract = Barrier(kPut, 100, steps, kUp, kOut, kDiscrete, 115)},
      {.name = "one-step up-out call",
       .contract = Barrier(kCall, 100, steps, kUp, kOut, kDiscrete, 125)},
      {.name = "one-step down-out put",
       .contract = Barrier(kPut, 105, steps, kDown, kOut, kDiscrete, 85)},
      {.name = "one-step down-in call",
       .contract = Barrier(kCall, 100, steps, kDown, kIn, kDiscrete, 90)},
      {.name = "bridge down-out call",
       .contract = Barrier(kCall, 100, steps, kDown, kOut, kContinuous, 90)},
      {.name = "bridge up-out put",
       .contract = Barrier(kPut, 100, steps, kUp, kOut, kContinuous, 115)},
      {.name = "bridge up-out call",
       .contract = Barrier(kCall, 100, steps, kUp, kOut, kContinuous, 130)},
      {.name = "bridge down-out put",
       .contract = Barrier(kPut, 105, steps, kDown, kOut, kContinuous, 85)},
      {.name = "bridge up-in put",
       .contract = Barrier(kPut, 100, steps, kUp, kIn, kContinuous, 115)},
  };
}

constexpr std::array<std::size_t, 2> kSpotVol = {kSpotInput, kVolInput};
constexpr std::array<std::size_t, 1> kSpotOnly = {kSpotInput};

struct Uniforms {
  explicit Uniforms(std::uint32_t steps) : normal(steps) {}
  void Fill(std::uint64_t path) { pricer::FillPath(kSeed, path, normal, {}); }
  [[nodiscard]] pricer::PathUniforms View() const { return {.normal = normal, .crossing = {}}; }
  std::vector<double> normal;
};

TEST(SecondOrder, ValueGradientAndLanesAreExact) {
  for (const pricer::Market<double>& market : {kMarket, kSecondMarket}) {
    for (const std::uint32_t steps : {1U, 3U, 12U}) {
      for (const Case& c : Cases(steps, market.spot)) {
        ASSERT_EQ(pricer::Validate(c.contract, market), pricer::ContractError::kNone) << c.name;
        ASSERT_EQ(pricer::ValidateConditional(c.contract, market), pricer::ConditionalError::kNone)
            << c.name;
        const pricer::GbmModel<double> model(market, steps);
        ConditionalPathGradient first(c.contract, market);
        HessianCheckPath<2> two(c.contract, market, kSpotVol);
        HessianCheckPath<1> one(c.contract, market, kSpotOnly);
        Uniforms u(steps);
        for (std::uint64_t path = 0; path < kPaths; ++path) {
          u.Fill(path);
          const auto value = pricer::ConditionalPayoff<double>(c.contract, model, u.View());
          const PathGradient::PathResult g = first.Evaluate(u.View());
          const HessianCheckPath<2>::Result h2 = two.Evaluate(u.View());
          const HessianCheckPath<1>::Result h1 = one.Evaluate(u.View());
          ASSERT_TRUE(std::isfinite(value)) << c.name << ", path " << path;
          ASSERT_EQ(Bits(g.value), Bits(value)) << c.name << ", path " << path;
          ASSERT_EQ(Bits(h2.value), Bits(value)) << c.name << ", path " << path;
          ASSERT_EQ(Bits(h1.value), Bits(value)) << c.name << ", path " << path;
          for (std::size_t i = 0; i < kInputs; ++i) {
            ASSERT_EQ(Bits(h2.gradient[i]), Bits(g.gradient[i]))
                << c.name << ", path " << path << ", input " << i;
            ASSERT_EQ(Bits(h1.gradient[i]), Bits(g.gradient[i]))
                << c.name << ", path " << path << ", input " << i;
            ASSERT_EQ(Bits(h1.rows[0][i]), Bits(h2.rows[0][i]))
                << c.name << ", path " << path << ", input " << i;
            ASSERT_TRUE(std::isfinite(h2.rows[0][i]) && std::isfinite(h2.rows[1][i]))
                << c.name << ", path " << path << ", input " << i;
          }
        }
      }
    }
  }
}

// The batch entry points agree with each other the same way.
TEST(SecondOrder, BatchEstimatesAreExact) {
  const pricer::RunConfig config{.seed = kSeed, .paths = 4096, .batches = 8, .threads = 3};
  for (const Case& c : Cases(12, kMarket.spot)) {
    const auto price = PriceConditional(c.contract, kMarket, config);
    const auto first = PriceWithConditionalGradient(c.contract, kMarket, config);
    const auto two = PriceWithHessian<2>(c.contract, kMarket, config, kSpotVol);
    const auto one = PriceWithHessian<1>(c.contract, kMarket, config, kSpotOnly);
    ASSERT_TRUE(price && first && two && one) << c.name;
    for (std::size_t b = 0; b < config.batches; ++b) {
      ASSERT_EQ(Bits(first->value.batch_means[b]), Bits(price->batch_means[b])) << c.name;
      ASSERT_EQ(Bits(two->value.batch_means[b]), Bits(price->batch_means[b])) << c.name;
      for (std::size_t i = 0; i < kInputs; ++i) {
        ASSERT_EQ(Bits(two->greeks[i].batch_means[b]), Bits(first->greeks[i].batch_means[b]))
            << c.name << ", input " << i;
        ASSERT_EQ(Bits(one->rows[0][i].batch_means[b]), Bits(two->rows[0][i].batch_means[b]))
            << c.name << ", input " << i;
      }
    }
  }
}

// The same bits for any thread count (EVIDENCE B11), as the first-order tape.
TEST(SecondOrder, ThreadCountChangesNoBit) {
  const Case c = Cases(12, kMarket.spot)[9];
  const auto base = PriceWithHessian<2>(
      c.contract, kMarket, {.seed = kSeed, .paths = 6000, .batches = 6, .threads = 1}, kSpotVol);
  ASSERT_TRUE(base);
  for (const std::uint32_t threads : {2U, 3U, 8U}) {
    const auto run = PriceWithHessian<2>(
        c.contract, kMarket, {.seed = kSeed, .paths = 6000, .batches = 6, .threads = threads},
        kSpotVol);
    ASSERT_TRUE(run);
    for (std::size_t k = 0; k < 2; ++k) {
      for (std::size_t i = 0; i < kInputs; ++i) {
        for (std::size_t b = 0; b < 6; ++b) {
          ASSERT_EQ(Bits(run->rows[k][i].batch_means[b]), Bits(base->rows[k][i].batch_means[b]))
              << threads << " threads";
        }
      }
    }
  }
}

// kNone where the run was admitted, so a missing refusal fails rather than reads error().
template <std::size_t N>
pricer::ConditionalError RefusalOf(
    const std::expected<HessianEstimate<N>, SecondOrderError>& result) {
  return result ? pricer::ConditionalError::kNone : result.error().conditional;
}

TEST(SecondOrder, RefusesTreatmentsWithNoSecondOrderForm) {
  using enum pricer::Treatment;
  const pricer::RunConfig config{.seed = kSeed, .paths = 64, .batches = 2, .threads = 1};
  pricer::Contract c = Barrier(pricer::OptionType::kCall, 100, 12, pricer::Direction::kDown,
                               pricer::Knock::kOut, pricer::Monitoring::kDiscrete, 90);
  c.barrier.treatment = kHard;
  EXPECT_EQ(RefusalOf(PriceWithHessian<1>(c, kMarket, config, kSpotOnly)),
            pricer::ConditionalError::kIndicator);
  c.barrier.treatment = kSmoothed;
  c.barrier.smoothing_width = 0.01;
  EXPECT_EQ(RefusalOf(PriceWithHessian<1>(c, kMarket, config, kSpotOnly)),
            pricer::ConditionalError::kSmoothed);
  c.barrier.monitoring = pricer::Monitoring::kContinuous;
  c.barrier.treatment = kHard;
  EXPECT_EQ(RefusalOf(PriceWithHessian<1>(c, kMarket, config, kSpotOnly)),
            pricer::ConditionalError::kIndicator);
  // A drift of 5 standard deviations per step on a continuous barrier.
  c.barrier.treatment = kBridge;
  const pricer::Market<double> steep{
      .spot = 100.0, .sigma = 0.01, .r = 0.6, .q = 0.0, .maturity = 1.0};
  c.steps = 1;
  ASSERT_EQ(pricer::Validate(c, steep), pricer::ContractError::kNone);
  EXPECT_EQ(RefusalOf(PriceWithHessian<1>(c, steep, config, kSpotOnly)),
            pricer::ConditionalError::kDriftPerStep);
}

pricer::Market<double> Bumped(pricer::Market<double> m, std::size_t input, double h) {
  switch (input) {
    case kSpotInput:
      m.spot += h;
      break;
    case kVolInput:
      m.sigma += h;
      break;
    default:
      break;
  }
  return m;
}

double InputOf(const pricer::Market<double>& m, std::size_t input) {
  return input == kSpotInput ? m.spot : m.sigma;
}

struct Gradients {
  std::vector<PathGradient::PathResult> paths;
  double x = 0.0;  // The bumped input's value.
};

Gradients GradientsAt(const pricer::Contract& c, const pricer::Market<double>& m,
                      std::uint64_t paths) {
  Gradients out;
  out.x = 0.0;
  ConditionalPathGradient tape(c, m);
  Uniforms u(c.steps);
  for (std::uint64_t path = 0; path < paths; ++path) {
    u.Fill(path);
    out.paths.push_back(tape.Evaluate(u.View()));
  }
  return out;
}

double FdRoundoff(const PathGradient::PathResult& up, const PathGradient::PathResult& down,
                  std::size_t input, double denominator) {
  const std::size_t statements = std::max(up.statements, down.statements);
  const std::size_t operands = std::max(up.operands, down.operands);
  const double n = (10.0 * static_cast<double>(statements)) + static_cast<double>(operands);
  return bounds::Gamma(n) * (up.absolute[input] + down.absolute[input]) / denominator;
}

struct Worst {
  double ratio = 0.0;
  double fd_error = 0.0;
};

void CheckDifferences(const Case& c, const pricer::Market<double>& market, Worst& worst) {
  HessianCheckPath<2> pass(c.contract, market, kSpotVol);
  std::vector<HessianCheckPath<2>::Result> base;
  Uniforms u(c.contract.steps);
  for (std::uint64_t path = 0; path < kPaths; ++path) {
    u.Fill(path);
    base.push_back(pass.Evaluate(u.View()));
  }
  for (std::size_t k = 0; k < kSpotVol.size(); ++k) {
    const std::size_t input = kSpotVol[k];
    const double h = input == kSpotInput ? market.spot * 0x1p-12 : 0x1p-14;
    const pricer::Market<double> up = Bumped(market, input, h);
    const pricer::Market<double> down = Bumped(market, input, -h);
    const pricer::Market<double> up2 = Bumped(market, input, h / 2);
    const pricer::Market<double> down2 = Bumped(market, input, -h / 2);
    const Gradients gu = GradientsAt(c.contract, up, kPaths);
    const Gradients gd = GradientsAt(c.contract, down, kPaths);
    const Gradients gu2 = GradientsAt(c.contract, up2, kPaths);
    const Gradients gd2 = GradientsAt(c.contract, down2, kPaths);
    const double den = InputOf(up, input) - InputOf(down, input);
    const double den2 = InputOf(up2, input) - InputOf(down2, input);
    for (std::uint64_t path = 0; path < kPaths; ++path) {
      const HessianCheckPath<2>::Result& r = base[path];
      for (std::size_t i = 0; i < kInputs; ++i) {
        const double d1 = (gu.paths[path].gradient[i] - gd.paths[path].gradient[i]) / den;
        const double d2 = (gu2.paths[path].gradient[i] - gd2.paths[path].gradient[i]) / den2;
        const double bound = std::fabs(d1 - d2) +
                             FdRoundoff(gu.paths[path], gd.paths[path], i, den) +
                             FdRoundoff(gu2.paths[path], gd2.paths[path], i, den2) +
                             HessianRoundoffBound(r.absolute_rows[k][i], r.statements, r.operands) +
                             (0x1p-50 * (std::fabs(d1) + std::fabs(d2))) + kAbsoluteFloor;
        const double error = std::fabs(d2 - r.rows[k][i]);
        ASSERT_TRUE(std::isfinite(r.rows[k][i])) << c.name << ", path " << path;
        ASSERT_LE(error, bound) << c.name << ", path " << path << ", lane " << k << ", input " << i
                                << ": forward over reverse " << r.rows[k][i] << ", D(h/2) " << d2
                                << ", D(h) " << d1;
        if (error > 0.0) worst.ratio = std::max(worst.ratio, error / bound);
        worst.fd_error = std::max(worst.fd_error, error);
      }
    }
  }
}

TEST(SecondOrder, CentralDifferencesOfTheTapesGradientAgreeWithinTheirBound) {
  for (const pricer::Market<double>& market : {kMarket, kSecondMarket}) {
    for (const std::uint32_t steps : {1U, 12U}) {
      for (const Case& c : Cases(steps, market.spot)) {
        Worst worst;
        CheckDifferences(c, market, worst);
        std::printf(
            "measured, %s, %u steps, spot %.0f: max |D(h/2) - H| / bound %.3g, max "
            "|D(h/2) - H| %.3g\n",
            c.name.c_str(), steps, market.spot, worst.ratio, worst.fd_error);
      }
    }
  }
}

TEST(SecondOrder, VannaIsSymmetricPerPathToRoundOff) {
  std::size_t checked = 0;
  for (const pricer::Market<double>& market : {kMarket, kSecondMarket}) {
    for (const std::uint32_t steps : {1U, 12U, 50U}) {
      for (const Case& c : Cases(steps, market.spot)) {
        HessianCheckPath<2> pass(c.contract, market, kSpotVol);
        Uniforms u(steps);
        double worst = 0.0;
        std::size_t below = 0;
        for (std::uint64_t path = 0; path < kPaths; ++path) {
          u.Fill(path);
          const HessianCheckPath<2>::Result r = pass.Evaluate(u.View());
          if (r.smallest_partial < kNormalFloor || r.smallest_adjoint < kNormalFloor) ++below;
          const double spot_vol = r.rows[0][kVolInput];
          const double vol_spot = r.rows[1][kSpotInput];
          const double bound =
              HessianRoundoffBound(r.absolute_rows[0][kVolInput], r.statements, r.operands) +
              HessianRoundoffBound(r.absolute_rows[1][kSpotInput], r.statements, r.operands) +
              kAbsoluteFloor;
          const double gap = std::fabs(spot_vol - vol_spot);
          ASSERT_LE(gap, bound) << c.name << ", path " << path << ": dS dvol " << spot_vol
                                << ", dvol dS " << vol_spot;
          if (gap > 0.0) worst = std::max(worst, gap / bound);
          ++checked;
        }
        std::printf(
            "measured, %s, %u steps, spot %.0f: max |vanna - vanna'| / bound %.3g, %zu paths with "
            "a lane below 2^-511\n",
            c.name.c_str(), steps, market.spot, worst, below);
      }
    }
  }
  EXPECT_GT(checked, 0U);
}

// The bound is not vacuous: on a long path it is far below one part in a million of the entry's
// absolute sum, the size of error a missing term makes.
TEST(SecondOrder, RoundoffBoundIsFarBelowAMissingTerm) {
  EXPECT_LT(HessianRoundoffBound(1.0, 5000, 8000), 1e-11);
  EXPECT_GT(HessianRoundoffBound(1.0, 5000, 8000), 0.0);
}

}  // namespace
}  // namespace aadp::ad
