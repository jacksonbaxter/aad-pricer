// The tape's derivative of the local-volatility price in every raw SVI parameter, against the
// forward-mode Dual of tests/support/, path by path (NOTES.md, "Stage 7a").
//
// Bound, stated before the comparison ran: bounds.hpp's AgreementBound, the existing tape bound,
//   |tape - dual| <= 2 gamma_n A (1 + 2 gamma_n) + 2^-1074 W (1 + 2 gamma_n) + 2^-1072,
// per parameter, with A the path's absolute gradient, W its underflow weight and n = 2 T + O from
// the path's own tape. Both compute the same pathwise derivative from the same partials (real.hpp
// writes each as tests/pricer/dual_ops.hpp does), so only association differs. The Dual has five
// tangents, so each path runs three times, one slice seeded each time. The values of the tape, the
// Dual and the double kernel must be the same bits.
// Added after the pre-registration, before it ran (see ComparePath): each difference also within
// 2^-26 of the path's largest |derivative|, because AgreementBound is vacuous on surface B at
// n = 4.
//
// Cases: surfaces A (free of static arbitrage) and B (crossing slices and density violations,
// which must floor on the checked paths), every product and treatment the pricer accepts,
// n = 1 and 4 sub-steps, 256 paths each. Then the batch estimate: its value is PriceLocalVol's
// bits, and value, gradient and tally are the same bits on 1 and 3 threads.
#include "ad/local_vol_gradient.hpp"

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

#include "ad/bounds.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/dual_ops.hpp"  // IWYU pragma: keep (tests/pricer/: the Dual overloads, by ADL)
#include "pricer/local_vol.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/runner.hpp"
#include "pricer/substep_kernels.hpp"
#include "support/dual.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"
#include "svi/synthetic_surface.hpp"

namespace aadp::ad {
namespace {

using pricer::Contract;
using pricer::Direction;
using pricer::Monitoring;
using pricer::OptionType;
using pricer::Product;
using pricer::Treatment;
using svi::testing::Surface;

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261117;
constexpr std::uint32_t kDates = 11;
constexpr std::uint64_t kPaths = 256;

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

// European or arithmetic Asian call, struck at 100, on kDates dates.
Contract Plain(Product product) {
  Contract c;
  c.product = product;
  c.strike = 100.0;
  c.steps = kDates;
  return c;
}

Contract Barrier(Direction d, Monitoring m, Treatment t, double level) {
  return {.product = Product::kBarrier,
          .type = OptionType::kCall,
          .strike = 100.0,
          .steps = kDates,
          .barrier = {.direction = d,
                      .knock = pricer::Knock::kOut,
                      .monitoring = m,
                      .treatment = t,
                      .level = level,
                      .smoothing_width = t == Treatment::kSmoothed ? 0.005 : 0.0}};
}

std::vector<std::pair<std::string, Contract>> Cases() {
  return {
      {"european", Plain(Product::kEuropean)},
      {"asian", Plain(Product::kAsianArithmetic)},
      {"doc-discrete-one-step",
       Barrier(Direction::kDown, Monitoring::kDiscrete, Treatment::kOneStep, 95.0)},
      {"doc-discrete-hard",
       Barrier(Direction::kDown, Monitoring::kDiscrete, Treatment::kHard, 95.0)},
      {"doc-discrete-smoothed",
       Barrier(Direction::kDown, Monitoring::kDiscrete, Treatment::kSmoothed, 95.0)},
      {"doc-continuous-bridge",
       Barrier(Direction::kDown, Monitoring::kContinuous, Treatment::kBridge, 95.0)},
      {"doc-continuous-sampled",
       Barrier(Direction::kDown, Monitoring::kContinuous, Treatment::kHard, 95.0)},
  };
}

// The surface on the Dual, slice `seeded`'s five parameters on the five tangents.
pricer::LocalVolModel<test::Dual> DualModel(const Surface& s, std::size_t seeded,
                                            const pricer::LocalVolContract& lv) {
  std::vector<svi::RawSvi<test::Dual>> params;
  for (std::size_t j = 0; j < s.params.size(); ++j) {
    const svi::RawSvi<double>& p = s.params[j];
    const auto x = [&](double v, std::size_t slot) {
      return j == seeded ? test::Dual::Variable(v, slot) : test::Dual(v);
    };
    params.push_back({.a = x(p.a, 0),
                      .b = x(p.b, 1),
                      .rho = x(p.rho, 2),
                      .m = x(p.m, 3),
                      .sigma = x(p.sigma, 4)});
  }
  return {svi::SviSurface<test::Dual>(s.terms, params),
          lv.maturity,
          lv.contract.steps * lv.substeps,
          {}};
}

struct Worst {
  double ratio = 0.0;
  double scaled = 0.0;  // max |tape - dual| / the path's largest |derivative|
  std::size_t statements = 0;
};

// The second, empirical check (NOTES.md, "Stage 7a", decisions after the pre-registration):
// AgreementBound sums absolute partials, which grow geometrically along a path where the local
// variance is steep in k, so on surface B at n = 4 and on the real surface it is far above the
// derivatives themselves and detects nothing. So each difference must also be within 2^-26 of the
// path's largest |derivative| over the 15. Not a proof: it is about 2,000 times the largest
// difference seen before it was written, 7.3e-12 of the parameter's own derivative, and a wrong
// or misplaced derivative is off by the order of the scale.
inline constexpr double kScaledAgreement = 0x1p-26;

// One path: the tape's value and gradient against the double kernel and the three seeded Duals.
void ComparePath(const LocalVolPathGradient::PathResult& result, double value,
                 const Contract& contract, std::uint32_t n,
                 const std::vector<pricer::LocalVolModel<test::Dual>>& duals,
                 const pricer::PathUniforms& u, const std::string& label, Worst& worst) {
  ASSERT_EQ(Bits(result.value), Bits(value)) << label;
  worst.statements = std::max(worst.statements, result.statements);
  std::vector<test::Dual> seeded;
  seeded.reserve(duals.size());
  double scale = 0.0;
  for (const pricer::LocalVolModel<test::Dual>& model : duals) {
    seeded.push_back(pricer::EvaluateSubstepped<test::Dual>(contract, n, model, u).value);
    ASSERT_EQ(Bits(seeded.back().value), Bits(value)) << label;
    for (std::size_t i = 0; i < 5; ++i) {
      scale = std::max(scale, std::fabs(seeded.back().tangent[i]));
    }
  }
  for (std::size_t j = 0; j < seeded.size(); ++j) {
    const test::Dual& dual = seeded[j];
    for (std::size_t i = 0; i < 5; ++i) {
      const std::size_t input = (5 * j) + i;
      const double bound = bounds::AgreementBound(
          result.absolute[input], result.operand_sums[input], result.statements, result.operands);
      const double diff = std::fabs(result.gradient[input] - dual.tangent[i]);
      ASSERT_TRUE(std::isfinite(result.gradient[input])) << label;
      ASSERT_LE(diff, bound) << label << ", parameter " << input << ": tape "
                             << result.gradient[input] << ", dual " << dual.tangent[i];
      ASSERT_LE(diff, kScaledAgreement * scale)
          << label << ", parameter " << input << ": tape " << result.gradient[input] << ", dual "
          << dual.tangent[i] << ", path scale " << scale;
      if (diff > 0.0) {
        worst.ratio = std::max(worst.ratio, diff / bound);
        worst.scaled = std::max(worst.scaled, diff / scale);
      }
    }
  }
}

TEST(LocalVolTapeVsDual, EveryParameterAgreesPerPathWithinTheTapeBound) {
  for (const auto& [surface_name, s] :
       {std::pair{"A", svi::testing::SurfaceA()}, std::pair{"B", svi::testing::SurfaceB()}}) {
    pricer::FloorTally floors;
    for (const auto& [name, contract] : Cases()) {
      for (const std::uint32_t n : {1U, 4U}) {
        const pricer::LocalVolContract lv{
            .contract = contract, .maturity = svi::testing::kExpiries[2], .substeps = n};
        ASSERT_EQ(pricer::ValidateLocalVol(lv, s.terms, s.params, {}).code,
                  pricer::LocalVolError::kNone);
        const std::uint32_t steps = kDates * n;
        pricer::LocalVolModel<double> model(svi::SviSurface<double>(s.terms, s.params), lv.maturity,
                                            steps, {});
        model.SetTally(&floors);
        std::vector<pricer::LocalVolModel<test::Dual>> duals;
        duals.reserve(3);
        for (std::size_t j = 0; j < 3; ++j) duals.push_back(DualModel(s, j, lv));
        LocalVolPathGradient tape(lv, s.terms, s.params, {});
        ASSERT_EQ(tape.Inputs(), 15U);
        std::vector<double> normal(steps);
        std::vector<double> crossing(steps);
        const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
        Worst worst;
        for (std::uint64_t path = 0; path < kPaths; ++path) {
          pricer::FillPath(kSeed, path, normal, crossing);
          const double value = pricer::EvaluateSubstepped<double>(contract, n, model, u).value;
          const LocalVolPathGradient::PathResult result = tape.Evaluate(u);
          ComparePath(result, value, contract, n, duals, u,
                      std::string(surface_name) + ", " + name + ", n " + std::to_string(n) +
                          ", path " + std::to_string(path),
                      worst);
          if (HasFatalFailure()) return;
        }
        std::printf(
            "measured, surface %s, %s, n %u, %llu paths: max |tape - dual| / bound %.3g, "
            "/ path scale %.3g (bound 2^-26), statements %zu\n",
            surface_name, name.c_str(), n, static_cast<unsigned long long>(kPaths), worst.ratio,
            worst.scaled, worst.statements);
      }
    }
    std::printf("surface %s: %llu of %llu checked path-steps floored, %llu capped\n", surface_name,
                static_cast<unsigned long long>(floors.Floored()),
                static_cast<unsigned long long>(floors.path_steps),
                static_cast<unsigned long long>(floors.Capped()));
    if (std::string(surface_name) == "B") {
      EXPECT_GT(floors.Floored(), 0U);
    }
  }
}

TEST(LocalVolGradient, TheValueIsThePricersAndNothingDependsOnTheThreadCount) {
  const Surface s = svi::testing::SurfaceB();
  for (const auto& [name, contract] : Cases()) {
    const pricer::LocalVolContract lv{
        .contract = contract, .maturity = svi::testing::kExpiries[2], .substeps = 4};
    const pricer::RunConfig one{.seed = kSeed, .paths = 4096, .batches = 8, .threads = 1};
    const auto price = pricer::PriceLocalVol(lv, s.terms, s.params, one);
    const auto grad = PriceLocalVolWithGradient(lv, s.terms, s.params, one);
    ASSERT_TRUE(price.has_value() && grad.has_value()) << name;
    ASSERT_EQ(grad->gradient.size(), 15U);
    EXPECT_EQ(Bits(grad->value.mean), Bits(price->value.mean)) << name;
    for (std::size_t b = 0; b < price->value.batch_means.size(); ++b) {
      EXPECT_EQ(Bits(grad->value.batch_means[b]), Bits(price->value.batch_means[b])) << name;
    }
    EXPECT_EQ(grad->floors.by_class, price->floors.by_class) << name;
    EXPECT_EQ(grad->floors.paths_with_floor, price->floors.paths_with_floor) << name;
    pricer::RunConfig three = one;
    three.threads = 3;
    const auto again = PriceLocalVolWithGradient(lv, s.terms, s.params, three);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(Bits(again->value.mean), Bits(grad->value.mean)) << name;
    for (std::size_t i = 0; i < 15; ++i) {
      EXPECT_EQ(Bits(again->gradient[i].mean), Bits(grad->gradient[i].mean)) << name << ' ' << i;
      EXPECT_EQ(Bits(again->gradient[i].standard_error), Bits(grad->gradient[i].standard_error))
          << name << ' ' << i;
      EXPECT_TRUE(std::isfinite(grad->gradient[i].mean)) << name << ' ' << i;
    }
    EXPECT_EQ(again->floors.by_class, grad->floors.by_class) << name;
  }
}

// The batch run sums each path's adjoints into the set-up and sweeps it once per block, so its
// gradient must be the mean of the per-path gradients Evaluate gives. Bound, stated before this
// check ran (NOTES.md, "Stage 7a", decisions after the pre-registration): 1e-10 times the mean
// of their absolute values, since only the order of the same additions differs, about 2^-52
// times a few thousand statements at most; a set-up sweep skipped, repeated or misread is off by
// whole multiples.
TEST(LocalVolGradient, TheBatchGradientIsTheMeanOfThePerPathGradients) {
  const Surface s = svi::testing::SurfaceB();
  constexpr std::uint64_t kRunPaths = 2048;
  for (const auto& [name, contract] : Cases()) {
    const pricer::LocalVolContract lv{
        .contract = contract, .maturity = svi::testing::kExpiries[2], .substeps = 4};
    const pricer::RunConfig run{.seed = kSeed, .paths = kRunPaths, .batches = 2, .threads = 1};
    const auto grad = PriceLocalVolWithGradient(lv, s.terms, s.params, run);
    ASSERT_TRUE(grad.has_value()) << name;
    LocalVolPathGradient tape(lv, s.terms, s.params, {});
    const std::uint32_t steps = kDates * lv.substeps;
    std::vector<double> normal(steps);
    std::vector<double> crossing(steps);
    const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
    std::vector<double> sum(15, 0.0);
    std::vector<double> absolute(15, 0.0);
    for (std::uint64_t path = 0; path < kRunPaths; ++path) {
      pricer::FillPath(kSeed, path, normal, crossing);
      const LocalVolPathGradient::PathResult r = tape.Evaluate(u);
      for (std::size_t i = 0; i < 15; ++i) {
        sum[i] += r.gradient[i];
        absolute[i] += std::fabs(r.gradient[i]);
      }
    }
    const auto paths = static_cast<double>(kRunPaths);
    for (std::size_t i = 0; i < 15; ++i) {
      const double mean = sum[i] / paths;
      EXPECT_LE(std::fabs(grad->gradient[i].mean - mean), 1e-10 * absolute[i] / paths)
          << name << ", parameter " << i << ": batch " << grad->gradient[i].mean << ", paths "
          << mean;
    }
  }
}

}  // namespace
}  // namespace aadp::ad
