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
//
// The tape against the Dual cannot see a floored variance that carries a derivative, since both
// record the same template. LocalVolFloors compares the tape with central differences of the
// double path instead, on surfaces where one floor class covers a wide region (below).
#include "ad/local_vol_gradient.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ad/bounds.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/dual_ops.hpp"  // IWYU pragma: keep (tests/pricer/: the Dual overloads, by ADL)
#include "pricer/local_vol.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/model.hpp"
#include "pricer/runner.hpp"
#include "pricer/substep_kernels.hpp"
#include "support/dual.hpp"
#include "svi/local_vol.hpp"
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

// A floored or capped local variance is a constant, so it carries no derivative
// (svi/local_vol.hpp). Per path, on common random numbers, the tape's 15 derivatives against
// central differences of the double path. Each surface below is surface A with one floor class
// over a wide region that every path, or most, crosses: B and the real surface floor too few
// path-steps for a price-level comparison to see a wrong derivative there (NOTES.md, "Stage 8a
// results").
//
// Tolerance, stated before the comparison first ran. Parameter theta_i is bumped by h_i =
// 2^-20 max(|theta_i|, 2^-7) and by h_i / 2, giving D_h and D_{h/2}. A path is compared only
// where every one of its steps keeps its class under all four bumps, and its payoff stays
// positive (strike 25), so that off those boundaries the path is a smooth function of theta. Then
//   |tape_i - D_{h/2,i}| <= 2 |D_{h,i} - D_{h/2,i}| + 2^-20 S,  S = max_i |D_{h/2,i}|.
// D_{h/2}'s truncation error is a quarter of D_h's, so |D_h - D_{h/2}| is three times it, and the
// first term covers it six times over, and the rounding of either difference where that
// dominates. The second term is the floor where the two agree by chance: rounding in a path of
// value about 75 is near 1e-12, so in D at h >= 7.5e-9 near 1.3e-4, against S of order 1e3.
// A floored step that kept its derivative adds the variance's adjoint times d(dT w / g), which
// at the floor, where sqrt is steep, is of order S itself.
// Coverage, stated with it: per product, at least 64 of the 256 paths compared and crossing the
// surface's class and no other floored class, and at most 32 left out.
class ClassRecorder {
 public:
  ClassRecorder(const Surface& s, const pricer::LocalVolContract& lv)
      : model_(svi::SviSurface<double>(s.terms, s.params), lv.maturity,
               lv.contract.steps * lv.substeps, {}) {
    model_.SetTally(&tally_);
  }
  ClassRecorder(const ClassRecorder&) = delete;
  ClassRecorder& operator=(const ClassRecorder&) = delete;
  ClassRecorder(ClassRecorder&&) = delete;
  ClassRecorder& operator=(ClassRecorder&&) = delete;
  ~ClassRecorder() = default;

  [[nodiscard]] const double& Spot() const { return model_.Spot(); }
  [[nodiscard]] const double& LogSpot() const { return model_.LogSpot(); }
  [[nodiscard]] const double& Discount() const { return model_.Discount(); }
  // The model's own Law; the class is read off the tally it adds to.
  [[nodiscard]] pricer::StepLaw<double> Law(std::uint32_t step, double x) const {
    const auto before = tally_.by_class;
    const pricer::StepLaw<double> law = model_.Law(step, x);
    for (std::size_t c = 0; c < before.size(); ++c) {
      if (tally_.by_class[c] != before[c]) {
        classes_.push_back(static_cast<svi::LocalVarianceClass>(c));
      }
    }
    return law;
  }

  // The path's value and the class of each of its steps.
  std::pair<double, std::vector<svi::LocalVarianceClass>> Run(const Contract& c, std::uint32_t n,
                                                              const pricer::PathUniforms& u) {
    classes_.clear();
    const double value = pricer::EvaluateSubstepped<double>(c, n, *this, u).value;
    return {value, classes_};
  }

 private:
  pricer::LocalVolModel<double> model_;
  mutable pricer::FloorTally tally_;
  mutable std::vector<svi::LocalVarianceClass> classes_;
};

double& Parameter(Surface& s, std::size_t input) {
  svi::RawSvi<double>& p = s.params[input / 5];
  switch (input % 5) {
    case 0:
      return p.a;
    case 1:
      return p.b;
    case 2:
      return p.rho;
    case 3:
      return p.m;
    default:
      return p.sigma;
  }
}

constexpr std::size_t kClasses = svi::kLocalVarianceClasses;

struct FloorStats {
  std::size_t compared = 0;
  std::size_t class_changed = 0;
  std::size_t payoff_not_positive = 0;
  std::array<std::size_t, kClasses> crossing{};  // compared paths with a step in each class
  std::array<std::size_t, kClasses> over{};      // ... and a derivative over the tolerance
  std::size_t target_only = 0;                   // crossing the target and no other floored class
  std::size_t target_only_over = 0;
  std::size_t over_paths = 0;  // compared paths with any derivative over the tolerance
  double worst = 0.0;          // max |tape - D_{h/2}| / tolerance
  std::string first;           // the first comparison over the tolerance
};

// Per parameter i, models 4 i to 4 i + 3 at theta + h, theta - h, theta + h / 2, theta - h / 2,
// and the widths (theta + h) - (theta - h) and the same at h / 2.
struct Bumps {
  std::vector<std::unique_ptr<ClassRecorder>> models;
  std::vector<std::array<double, 2>> width;
};

void MakeBumps(const Surface& s, const pricer::LocalVolContract& lv, Bumps& out) {
  for (std::size_t i = 0; i < 15; ++i) {
    Surface probe = s;
    const double theta = Parameter(probe, i);
    const double h = 0x1p-20 * std::max(std::fabs(theta), 0x1p-7);
    std::array<double, 2> w{};
    for (std::size_t half = 0; half < 2; ++half) {
      const double step = half == 0 ? h : 0.5 * h;
      const double up = theta + step;
      const double down = theta - step;
      w[half] = up - down;
      for (const double x : {up, down}) {
        Parameter(probe, i) = x;
        ASSERT_EQ(pricer::ValidateLocalVol(lv, probe.terms, probe.params, {}).code,
                  pricer::LocalVolError::kNone);
        out.models.push_back(std::make_unique<ClassRecorder>(probe, lv));
      }
    }
    out.width.push_back(w);
  }
}

// One compared path's 15 derivatives against the tolerance above. Returns whether any is over it.
bool OverTolerance(const std::vector<double>& gradient, const std::vector<double>& bumped_value,
                   const Bumps& bumps, const std::string& label, FloorStats& stats) {
  std::array<double, 15> d_h{};
  std::array<double, 15> d_half{};
  double scale = 0.0;
  for (std::size_t i = 0; i < 15; ++i) {
    const double* v = &bumped_value[4 * i];
    d_h[i] = (v[0] - v[1]) / bumps.width[i][0];
    d_half[i] = (v[2] - v[3]) / bumps.width[i][1];
    scale = std::max(scale, std::fabs(d_half[i]));
  }
  bool over = false;
  for (std::size_t i = 0; i < 15; ++i) {
    const double tolerance = (2.0 * std::fabs(d_h[i] - d_half[i])) + (0x1p-20 * scale);
    const double diff = std::fabs(gradient[i] - d_half[i]);
    if (!(diff <= tolerance)) {
      if (stats.first.empty()) {
        stats.first = label + ", parameter " + std::to_string(i) + ": tape " +
                      std::to_string(gradient[i]) + ", D_h/2 " + std::to_string(d_half[i]) +
                      ", D_h " + std::to_string(d_h[i]) + ", tolerance " +
                      std::to_string(tolerance);
      }
      over = true;
    }
    if (tolerance > 0.0) stats.worst = std::max(stats.worst, diff / tolerance);
  }
  return over;
}

// Counts a compared path by the classes its steps crossed.
void CountPath(const std::vector<svi::LocalVarianceClass>& classes, svi::LocalVarianceClass target,
               bool over, FloorStats& stats) {
  std::array<bool, kClasses> seen{};
  for (const svi::LocalVarianceClass c : classes) seen[static_cast<std::size_t>(c)] = true;
  const auto target_index = static_cast<std::size_t>(target);
  bool other_floor = false;
  for (std::size_t c = 0; c < kClasses; ++c) {
    if (!seen[c]) continue;
    stats.crossing[c] += 1;
    if (over) stats.over[c] += 1;
    other_floor = other_floor ||
                  (c != target_index && svi::IsFloored(static_cast<svi::LocalVarianceClass>(c)));
  }
  stats.compared += 1;
  if (over) stats.over_paths += 1;
  if (seen[target_index] && !other_floor) {
    stats.target_only += 1;
    if (over) stats.target_only_over += 1;
  }
}

void Report(const char* surface_name, const char* product_name, svi::LocalVarianceClass target,
            std::uint64_t paths, const FloorStats& stats) {
  std::string by_class;
  for (std::size_t c = 0; c < kClasses; ++c) {
    if (stats.crossing[c] == 0) continue;
    by_class += std::string(" ") +
                svi::LocalVarianceClassName(static_cast<svi::LocalVarianceClass>(c)) + " " +
                std::to_string(stats.over[c]) + "/" + std::to_string(stats.crossing[c]);
  }
  std::printf(
      "measured, floors %s, %s: %zu of %llu paths compared (%zu class changed, %zu payoff not "
      "positive); over tolerance / crossing, by class:%s; %s only: %zu/%zu; worst |tape - D_h/2| "
      "/ tolerance %.3g\n",
      surface_name, product_name, stats.compared, static_cast<unsigned long long>(paths),
      stats.class_changed, stats.payoff_not_positive, by_class.c_str(),
      svi::LocalVarianceClassName(target), stats.target_only_over, stats.target_only, stats.worst);
}

void CompareFloors(const char* surface_name, const Surface& s, svi::LocalVarianceClass target) {
  constexpr std::uint32_t kSubsteps = 4;
  constexpr std::uint64_t kFloorPaths = 256;
  for (const Product product : {Product::kEuropean, Product::kAsianArithmetic}) {
    const char* product_name = product == Product::kEuropean ? "european" : "asian";
    Contract contract = Plain(product);
    contract.strike = 25.0;
    const pricer::LocalVolContract lv{
        .contract = contract, .maturity = svi::testing::kExpiries[2], .substeps = kSubsteps};
    ASSERT_EQ(pricer::ValidateLocalVol(lv, s.terms, s.params, {}).code,
              pricer::LocalVolError::kNone);
    ClassRecorder base(s, lv);
    Bumps bumps;
    MakeBumps(s, lv, bumps);
    if (::testing::Test::HasFatalFailure()) return;
    LocalVolPathGradient tape(lv, s.terms, s.params, {});
    const std::uint32_t steps = kDates * kSubsteps;
    std::vector<double> normal(steps);
    std::vector<double> crossing(steps);
    const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
    std::vector<double> bumped_value(bumps.models.size());
    FloorStats stats;
    for (std::uint64_t path = 0; path < kFloorPaths; ++path) {
      pricer::FillPath(kSeed, path, normal, crossing);
      const auto [value, classes] = base.Run(contract, kSubsteps, u);
      const LocalVolPathGradient::PathResult result = tape.Evaluate(u);
      const std::string label =
          std::string(surface_name) + ", " + product_name + ", path " + std::to_string(path);
      ASSERT_EQ(Bits(result.value), Bits(value)) << label;
      bool same_classes = true;
      bool positive = value > 0.0;
      for (std::size_t b = 0; b < bumps.models.size(); ++b) {
        const auto [v, c] = bumps.models[b]->Run(contract, kSubsteps, u);
        bumped_value[b] = v;
        same_classes = same_classes && c == classes;
        positive = positive && v > 0.0;
      }
      if (!same_classes) {
        stats.class_changed += 1;
      } else if (!positive) {
        stats.payoff_not_positive += 1;
      } else {
        CountPath(classes, target,
                  OverTolerance(result.gradient, bumped_value, bumps, label, stats), stats);
      }
    }
    Report(surface_name, product_name, target, kFloorPaths, stats);
    EXPECT_GE(stats.target_only, 64U) << surface_name << ", " << product_name;
    EXPECT_LE(stats.class_changed + stats.payoff_not_positive, 32U)
        << surface_name << ", " << product_name;
    EXPECT_EQ(stats.over_paths, 0U) << stats.first;
  }
}

// Surface A's slices, with one changed as the class needs.
Surface FloorSurface(const std::array<svi::RawSvi<double>, 3>& params) {
  return svi::testing::WithTerms(params, svi::testing::kForwards, svi::testing::kDiscounts);
}

constexpr svi::RawSvi<double> kA0 = svi::testing::kParamsA[0];
constexpr svi::RawSvi<double> kA1 = svi::testing::kParamsA[1];
constexpr svi::RawSvi<double> kA2 = svi::testing::kParamsA[2];

constexpr svi::RawSvi<double> WithA(svi::RawSvi<double> p, double a) {
  p.a = a;
  return p;
}

// a_0 = -4e-4 puts w_0 below zero near the money, so every path's steps before the first slice
// are floored there.
TEST(LocalVolFloors, NonPositiveVarianceCarriesNoDerivative) {
  CompareFloors("non-positive", FloorSurface({WithA(kA0, -4e-4), kA1, kA2}),
                svi::LocalVarianceClass::kNonPositiveVariance);
}

// The second slice is the first lowered by 1e-4: dT w < 0 over the whole second segment.
TEST(LocalVolFloors, CalendarFloorCarriesNoDerivative) {
  CompareFloors("calendar", FloorSurface({kA0, WithA(kA0, kA0.a - 1e-4), kA2}),
                svi::LocalVarianceClass::kCalendar);
}

// The money in a steep right wing on every slice, w' near b (1 + rho) = 0.4 against w near 0.01 at
// k = 0 on the first: w' q / 4 = w'^2 / (4 w) passes 1, and g < 0 over a wide band of k.
TEST(LocalVolFloors, ButterflyFloorCarriesNoDerivative) {
  constexpr svi::RawSvi<double> kWing = {.a = 0.0, .b = 0.25, .rho = 0.6, .m = -0.1, .sigma = 0.01};
  CompareFloors("butterfly",
                FloorSurface({WithA(kWing, -0.03), WithA(kWing, -0.025), WithA(kWing, -0.015)}),
                svi::LocalVarianceClass::kButterfly);
}

// The second slice is the first raised by 5e-7: dT w = 2.6e-5 per year, below 1e-4 g.
TEST(LocalVolFloors, BelowFloorCarriesNoDerivative) {
  CompareFloors("below-floor", FloorSurface({kA0, WithA(kA0, kA0.a + 5e-7), kA2}),
                svi::LocalVarianceClass::kBelowFloor);
}

// The second slice raised by 0.1 and the third by 0.12: dT w near 5 per year, above 4 g.
TEST(LocalVolFloors, CapCarriesNoDerivative) {
  CompareFloors("capped", FloorSurface({kA0, WithA(kA1, 0.1), WithA(kA2, 0.12)}),
                svi::LocalVarianceClass::kCapped);
}

}  // namespace
}  // namespace aadp::ad
