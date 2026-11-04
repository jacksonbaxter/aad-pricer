// Gradients on the vector tape against the scalar tape, at every lane width this build can run
// (NOTES.md, "Vector tape: design and oracle", oracle items 1, 2, 4 and 5). No tolerance:
// 1. per path: every lane's value and gradient after its own set-up sweep are
//    PathGradient::Evaluate's for that path, on every product and treatment of tests/ad/cases.hpp
//    at 1, 12 and 50 steps, and on stage 5a's wild and dying markets;
// 2. block sums: PriceWithGradientLanes gives PriceWithGradient's estimate (value and Greeks:
//    mean, SE, every batch mean) on 1, 2, 3 and 8 threads, with no fallback;
// 4. the dot product on priced paths: each lane's two sides are the scalar tape's bits, and within
//    tests/ad/dot_product_test.cpp's bound;
// 5. a kernel whose Select splits lanes between two active statements falls back, is counted
//    exactly, and still gives the scalar gradient's bits.
#include "simd/lane_gradient.hpp"

#include <gtest/gtest.h>
#include <hwy/highway.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "ad/bounds.hpp"
#include "ad/cases.hpp"
#include "ad/gradient.hpp"
#include "ad/real.hpp"
#include "ad/tape.hpp"
#include "lane_widths.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "simd/lane_kernels.hpp"
#include "simd/lane_path_gradient.hpp"
#include "simd/lane_real.hpp"
#include "simd/lane_runner.hpp"
#include "simd/lane_tape.hpp"
#include "simd/lane_tape_ops.hpp"
#include "simd/simd_pricer.hpp"

#ifndef AADP_SMOKE_SIZE
#define AADP_SMOKE_SIZE 0
#endif

namespace aadp::simd {
namespace {

namespace hn = hwy::HWY_NAMESPACE;
using ad::kInputs;
using pricer::Contract;
using pricer::Market;
using testing::ForEachWidth;
using testing::SameBits;

// Development seeds, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261122;
constexpr std::uint64_t kDotSeed = 20261123;
constexpr std::uint64_t kPaths = AADP_SMOKE_SIZE ? 32 : 256;
// Not a multiple of any width, so lane 0 is not always an even path.
constexpr std::uint64_t kFirstPath = 4093;

struct NamedMarket {
  const char* name;
  Market<double> market;
  std::uint32_t steps;
};

// tests/ad/cases.hpp's market at 1, 12 and 50 steps, and stage 5a's wild and dying markets: the
// dying one drives every one-step path through the barrier, so the guard below 2^-1000 and the
// dead-path Selects run.
constexpr auto kMarkets = std::to_array<NamedMarket>({
    {.name = "cases", .market = ad::testing::kMarket, .steps = 1},
    {.name = "cases", .market = ad::testing::kMarket, .steps = 12},
    {.name = "cases", .market = ad::testing::kMarket, .steps = 50},
    {.name = "wild",
     .market = {.spot = 100.0, .sigma = 1.2, .r = 0.1, .q = 0.0, .maturity = 2.0},
     .steps = 7},
    {.name = "dying",
     .market = {.spot = 100.0, .sigma = 1e-4, .r = -1.0, .q = 1.0, .maturity = 1.0},
     .steps = 12},
});

struct ScalarPath {
  double value = 0.0;
  std::array<double, kInputs> gradient{};
};

std::vector<ScalarPath> ScalarPaths(const Contract& c, const Market<double>& market,
                                    std::uint64_t first, std::uint64_t count) {
  std::vector<ScalarPath> out;
  ad::PathGradient path(c, market);
  std::vector<double> normal(c.steps);
  std::vector<double> crossing(c.steps);
  const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
  for (std::uint64_t p = first; p < first + count; ++p) {
    pricer::FillPath(kSeed, p, normal, crossing);
    const ad::PathGradient::PathResult r = path.Evaluate(u);
    out.push_back({.value = r.value, .gradient = r.gradient});
  }
  return out;
}

template <class D>
struct LaneBuffers {
  explicit LaneBuffers(const Contract& c)
      : normal(c.steps), crossing(pricer::NeedsCrossingUniforms(c) ? c.steps : 0) {}
  void Fill(std::uint64_t seed, std::uint64_t first) {
    FillLanes(D(), seed, first, std::span(normal), std::span(crossing));
  }
  [[nodiscard]] LaneUniforms<LaneReal<D>> Uniforms() const {
    return {.normal = normal, .crossing = crossing};
  }
  std::vector<LaneReal<D>> normal;
  std::vector<LaneReal<D>> crossing;
};

struct Tally {
  std::size_t paths = 0;
  std::size_t mismatches = 0;
  std::size_t fallbacks = 0;
};

template <class D>
void ComparePaths(const Contract& c, const Market<double>& market, const std::string& name,
                  Tally& tally) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  const std::vector<ScalarPath> scalar = ScalarPaths(c, market, kFirstPath, kPaths);
  LanePathGradient<D> lanes(c, market);
  LaneBuffers<D> buffers(c);
  for (std::uint64_t group = 0; group + kLanes <= kPaths; group += kLanes) {
    buffers.Fill(kSeed, kFirstPath + group);
    const auto result = lanes.Evaluate(buffers.Uniforms());
    if (result.fell_back) ++tally.fallbacks;
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      const ScalarPath& want = scalar[group + lane];
      ++tally.paths;
      bool same = SameBits(result.value[lane], want.value);
      for (std::size_t k = 0; k < kInputs; ++k) {
        same = same && SameBits(result.gradient[lane][k], want.gradient[k]);
      }
      if (!same && tally.mismatches++ < 5) {
        ADD_FAILURE() << name << ", " << kLanes << " lanes, path " << kFirstPath + group + lane
                      << ": value " << result.value[lane] << " delta " << result.gradient[lane][0]
                      << " vega " << result.gradient[lane][1] << "; scalar tape " << want.value
                      << ", " << want.gradient[0] << ", " << want.gradient[1];
      }
    }
  }
  EXPECT_EQ(lanes.fallback_groups(), 0U) << name;
}

TEST(LaneGradient, PerPathGradientsAreTheScalarTapesBits) {
  for (const NamedMarket& m : kMarkets) {
    const auto cases = ad::testing::AllCases(m.steps);
    ForEachWidth([&](auto d) {
      using D = decltype(d);
      Tally tally;
      for (const ad::testing::Case& c : cases) {
        ASSERT_EQ(pricer::Validate(c.contract, m.market), pricer::ContractError::kNone) << c.name;
        ComparePaths<D>(c.contract, m.market,
                        c.name + " on " + m.name + " at " + std::to_string(m.steps) + " steps",
                        tally);
      }
      std::printf(
          "measured, %zu lanes, %s market at %u steps: %zu cases, %zu paths, %zu not "
          "bit-identical to the scalar tape, %zu groups fell back\n",
          hn::MaxLanes(D()), m.name, m.steps, cases.size(), tally.paths, tally.mismatches,
          tally.fallbacks);
      EXPECT_EQ(tally.mismatches, 0U);
      EXPECT_EQ(tally.fallbacks, 0U);
    });
  }
}

// dot_product_test.cpp's bound, from the scalar tape's absolute sweeps.
double DotBound(double s, double w, std::size_t statements, std::size_t operands) {
  const double n = ad::bounds::RoundoffTerms(statements, operands) + 8.0;
  const double gamma = ad::bounds::Gamma(n);
  return (((2.0 * gamma * s) + ad::bounds::UnderflowTerm(w + 8.0)) * (1.0 + (2.0 * gamma))) +
         ad::bounds::kBoundRounding;
}

struct DotSides {
  double lhs = 0.0;
  double rhs = 0.0;
  double bound = 0.0;
};

// dot_product_test.cpp's HoldsOnWholePricedPathsOfEveryProduct on one path.
DotSides ScalarDot(const Contract& c, const Market<double>& market, std::uint64_t p,
                   const std::array<double, kInputs>& direction, double seed) {
  DotSides out;
  ad::PathGradient path(c, market);
  ad::Tape& tape = path.tape();
  std::vector<double> normal(c.steps);
  std::vector<double> crossing(c.steps);
  pricer::FillPath(kDotSeed, p, normal, crossing);
  const ad::Real payoff = path.Record({.normal = normal, .crossing = crossing});
  const ad::Position end = tape.GetPosition();
  const ad::MemoryReport memory = tape.Memory();
  for (std::size_t k = 0; k < kInputs; ++k) tape.SetTangent(path.inputs()[k], direction[k]);
  tape.SweepForward(ad::Position{});
  out.lhs = seed * tape.Tangent(payoff);
  tape.AddAdjoint(payoff, seed);
  tape.SweepBack(end, ad::Position{});
  for (std::size_t k = 0; k < kInputs; ++k) {
    out.rhs += direction[k] * tape.Adjoint(path.inputs()[k]);
  }
  tape.ClearAdjoints(end, ad::Position{});
  tape.AddAdjoint(payoff, std::fabs(seed));
  tape.SweepBackAbsolute(end, ad::Position{});
  double s = 0.0;
  for (std::size_t k = 0; k < kInputs; ++k) {
    s += std::fabs(direction[k]) * tape.Adjoint(path.inputs()[k]);
  }
  tape.ClearTangents();
  for (std::size_t k = 0; k < kInputs; ++k) {
    tape.SetTangent(path.inputs()[k], std::fabs(direction[k]));
  }
  tape.SweepForwardAbsolute(ad::Position{});
  const ad::Tape::OperandSums sums = tape.SumOverOperands(end, ad::Position{});
  out.bound = DotBound(s, sums.adjoints + sums.tangents, memory.statements, memory.operands);
  return out;
}

template <std::size_t N>
struct LaneDotSides {
  std::array<double, N> lhs{};
  std::array<double, N> rhs{};
  bool divergent = false;
};

// Each lane's two sides on one group: the lane tape swept forward from the scalar set-up's
// tangents, then back, each lane replayed into the set-up and swept there.
template <class D>
LaneDotSides<hn::MaxLanes(D())> LaneDot(D d, const Contract& c, std::uint64_t first,
                                        const std::array<double, kInputs>& direction,
                                        const std::array<double, hn::MaxLanes(D())>& seeds) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  LaneDotSides<kLanes> out;
  LanePathGradient<D> lanes(c, ad::testing::kMarket);
  LaneBuffers<D> buffers(c);
  buffers.Fill(kDotSeed, first);
  ad::Tape& scalar = lanes.scalar().tape();
  for (std::size_t k = 0; k < kInputs; ++k) {
    scalar.SetTangent(lanes.scalar().inputs()[k], direction[k]);
  }
  scalar.SweepForward(ad::Position{});
  LaneTape<D>& tape = lanes.tape();
  const LaneTapeReal<D> payoff = lanes.Record(buffers.Uniforms());
  out.divergent = tape.Divergent();
  if (out.divergent) return out;
  tape.SweepForward(lanes.mark(), [&](ad::Index i) { return scalar.TangentAt(i); });
  const hn::Vec<D> seed = hn::LoadU(d, seeds.data());
  hn::StoreU(hn::Mul(seed, tape.Tangent(payoff)), d, out.lhs.data());
  scalar.ClearTangents();
  tape.ClearTangents();
  tape.AddAdjoint(payoff, seed);
  tape.SweepBack(tape.GetPosition(), lanes.mark());
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    lanes.Replay(lane);
    const std::array<double, kInputs> gradient = lanes.SweepSetup();
    for (std::size_t k = 0; k < kInputs; ++k) out.rhs[lane] += direction[k] * gradient[k];
  }
  lanes.ClearGroup();
  return out;
}

struct DotTally {
  std::size_t checked = 0;
  std::size_t mismatches = 0;
  double worst = 0.0;
};

template <class D>
void CheckDotGroup(D d, const ad::testing::Case& c, std::uint32_t steps, std::uint64_t first,
                   std::mt19937_64& random, DotTally& tally) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  std::uniform_real_distribution<double> unit(-1.0, 1.0);
  std::array<double, kInputs> direction{};
  for (double& x : direction) x = unit(random);
  std::array<double, kLanes> seeds{};
  for (double& x : seeds) x = unit(random);
  std::array<DotSides, kLanes> want{};
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    want[lane] = ScalarDot(c.contract, ad::testing::kMarket, first + lane, direction, seeds[lane]);
  }
  const LaneDotSides<kLanes> got = LaneDot(d, c.contract, first, direction, seeds);
  ASSERT_FALSE(got.divergent) << c.name;
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    ++tally.checked;
    const double lhs = got.lhs[lane];
    const double rhs = got.rhs[lane];
    if ((!SameBits(lhs, want[lane].lhs) || !SameBits(rhs, want[lane].rhs)) &&
        tally.mismatches++ < 5) {
      ADD_FAILURE() << c.name << ", " << steps << " steps, " << kLanes << " lanes, path "
                    << first + lane << ": lane <y', y-bar> " << lhs << ", <x', x-bar> " << rhs
                    << "; scalar tape " << want[lane].lhs << ", " << want[lane].rhs;
    }
    const double diff = std::fabs(lhs - rhs);
    EXPECT_LE(diff, want[lane].bound) << c.name << ", path " << first + lane;
    if (diff > 0.0) tally.worst = std::max(tally.worst, diff / want[lane].bound);
  }
}

TEST(LaneGradient, DotProductOnPricedPathsIsTheScalarTapesBits) {
  ForEachWidth([](auto d) {
    constexpr std::size_t kLanes = hn::MaxLanes(decltype(d)());
    constexpr std::uint64_t kGroups = AADP_SMOKE_SIZE ? 2 : 8;
    std::mt19937_64 random(kDotSeed);
    DotTally tally;
    for (const std::uint32_t steps : {1U, 12U, 50U}) {
      for (const ad::testing::Case& c : ad::testing::AllCases(steps)) {
        for (std::uint64_t g = 0; g < kGroups; ++g) {
          CheckDotGroup(d, c, steps, g * kLanes, random, tally);
        }
      }
    }
    std::printf(
        "measured, %zu lanes: dot product on %zu priced paths at 1, 12 and 50 steps, %zu not the "
        "scalar tape's bits; max |difference| / bound %.3g\n",
        kLanes, tally.checked, tally.mismatches, tally.worst);
    EXPECT_EQ(tally.mismatches, 0U);
  });
}

bool SameEstimate(const pricer::Estimate& a, const pricer::Estimate& b) {
  if (!SameBits(a.mean, b.mean) || !SameBits(a.standard_error, b.standard_error)) return false;
  if (a.batch_means.size() != b.batch_means.size()) return false;
  for (std::size_t i = 0; i < a.batch_means.size(); ++i) {
    if (!SameBits(a.batch_means[i], b.batch_means[i])) return false;
  }
  return true;
}

bool SameGradient(const ad::GradientEstimate& a, const ad::GradientEstimate& b) {
  bool same = SameEstimate(a.value, b.value);
  for (std::size_t k = 0; k < kInputs; ++k) same = same && SameEstimate(a.greeks[k], b.greeks[k]);
  return same;
}

// Each batch is three blocks and a part block of 17 paths (one block and 17 at smoke size), so a
// group runs past a block's end at every width above one, and workers take blocks out of order.
constexpr std::uint32_t kBatches = AADP_SMOKE_SIZE ? 2 : 4;
constexpr std::uint64_t kRunPaths =
    kBatches * (((AADP_SMOKE_SIZE ? 1 : 3) * pricer::kBlockPaths) + 17);
constexpr std::array<std::uint32_t, 4> kThreads = {1, 2, 3, 8};

TEST(LaneGradient, BlockSumsAreTheScalarGradientsBitsForEveryWidthAndThreadCount) {
  std::size_t runs = 0;
  const auto cases = ad::testing::AllCases(12);
  for (const ad::testing::Case& c : cases) {
    const pricer::RunConfig base{.seed = kSeed, .paths = kRunPaths, .batches = kBatches};
    const auto scalar = ad::PriceWithGradient(c.contract, ad::testing::kMarket, base);
    ASSERT_TRUE(scalar.has_value()) << c.name;
    for (const std::size_t lanes : LaneWidths()) {
      for (const std::uint32_t threads : kThreads) {
        pricer::RunConfig config = base;
        config.threads = threads;
        const auto got = PriceWithGradientLanes(c.contract, ad::testing::kMarket, config, lanes);
        ASSERT_TRUE(got.has_value()) << c.name;
        ++runs;
        EXPECT_TRUE(SameGradient(got->estimate, *scalar))
            << c.name << ", " << lanes << " lanes, " << threads << " threads: delta "
            << got->estimate.Of(ad::Greek::kDelta).mean << " against "
            << scalar->Of(ad::Greek::kDelta).mean;
        EXPECT_EQ(got->fallback_groups, 0U) << c.name << ", " << lanes << " lanes";
        EXPECT_EQ(got->fallback_paths, 0U) << c.name << ", " << lanes << " lanes";
      }
    }
  }
  std::printf(
      "measured: %zu cases, widths up to %zu, %zu runs of %llu paths on the vector tape, each "
      "compared with PriceWithGradient\n",
      cases.size(), MaxLanes(), runs, static_cast<unsigned long long>(kRunPaths));
}

TEST(LaneGradient, RefusesWhatPriceWithGradientRefusesAndUnknownWidths) {
  const auto cases = ad::testing::AllCases(12);
  const Contract& c = cases.front().contract;
  const pricer::RunConfig ok{.seed = kSeed, .paths = 64, .batches = 2, .threads = 1};
  Market<double> bad = ad::testing::kMarket;
  bad.sigma = -1.0;
  const auto refused = PriceWithGradientLanes(c, bad, ok, 1);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().price.contract, pricer::ContractError::kVolatility);
  pricer::RunConfig one_batch = ok;
  one_batch.batches = 1;
  const auto run = PriceWithGradientLanes(c, ad::testing::kMarket, one_batch, 1);
  ASSERT_FALSE(run.has_value());
  EXPECT_EQ(run.error().price.run, pricer::RunError::kBatches);
  const auto width = PriceWithGradientLanes(c, ad::testing::kMarket, ok, 3);
  ASSERT_FALSE(width.has_value());
  EXPECT_TRUE(width.error().lanes);
}

// A kernel no pricer product has: at the end, a Select between two different active statements,
// on the first uniform. Lanes whose first uniforms fall on both sides of one half diverge.
struct DivergingKernel {
  template <class Real, class Model>
  [[nodiscard]] Real Lanes(const Contract& c, const Model& model,
                           const LaneUniforms<Real>& u) const {
    Real x = model.LogSpot();
    for (std::uint32_t i = 0; i < c.steps; ++i) {
      const pricer::StepLaw<Real> law = model.Law(i, x);
      x = law.mean + law.sd * InverseNormal(u.normal[i]);
    }
    const Real a = Exp(x);
    const Real b = a * 1.5;
    return model.Discount() * Select(u.normal[0] < 0.5, a, b);
  }
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static): a Kernel may carry state.
  [[nodiscard]] ad::Real Scalar(const Contract& c, const pricer::GbmModel<ad::Real>& model,
                                const pricer::PathUniforms& u) const {
    ad::Real x = model.LogSpot();
    for (std::uint32_t i = 0; i < c.steps; ++i) {
      const pricer::StepLaw<ad::Real> law = model.Law(i, x);
      x = law.mean + law.sd * InverseNormal(ad::Real(u.normal[i]));
    }
    const ad::Real a = Exp(x);
    const ad::Real b = a * 1.5;
    return model.Discount() * Select(u.normal[0] < 0.5, a, b);
  }
};

// RunBatches on the scalar tape with the same kernel, as ad::GradientPathFn runs it.
class ScalarKernelFn {
 public:
  ScalarKernelFn(const Contract& c, const Market<double>& m) : contract_(c), path_(c, m) {}
  void operator()(const pricer::PathUniforms& u, std::array<double, ad::kGradientStats>& stats) {
    stats = {};
    ad::Tape& tape = path_.tape();
    const ad::Real payoff = DivergingKernel{}.Scalar(contract_, path_.model(), u);
    tape.AddAdjoint(payoff, 1.0);
    tape.SweepBack(tape.GetPosition(), path_.mark());
    path_.Rewind();
    stats[0] = payoff.value();
  }
  void EndBlock(std::array<double, ad::kGradientStats>& sum) {
    const std::array<double, kInputs> gradient = path_.SweepSetup();
    for (std::size_t i = 0; i < kInputs; ++i) {
      sum[1 + i] += i == ad::kMaturityInput ? -gradient[i] : gradient[i];
    }
  }

 private:
  Contract contract_;
  ad::PathGradient path_;
};

// The groups RunLaneBatches forms, and which of them split: every lane is computed, past the
// block's end too, so any lane can split a group; only the lanes inside the block are counted as
// paths.
struct ExpectedFallbacks {
  std::uint64_t groups = 0;
  std::uint64_t paths = 0;
};

ExpectedFallbacks CountSplits(const pricer::RunConfig& config, std::size_t lanes) {
  ExpectedFallbacks out;
  const std::uint64_t per_batch = config.paths / config.batches;
  for (std::uint64_t batch = 0; batch < config.batches; ++batch) {
    for (std::uint64_t first = batch * per_batch; first < (batch + 1) * per_batch;
         first += pricer::kBlockPaths) {
      const std::uint64_t last = std::min(first + pricer::kBlockPaths, (batch + 1) * per_batch);
      for (std::uint64_t group = first; group < last; group += lanes) {
        std::size_t below = 0;
        for (std::size_t lane = 0; lane < lanes; ++lane) {
          if (pricer::DrawStep(config.seed, group + lane, 0).normal < 0.5) ++below;
        }
        if (below != 0 && below != lanes) {
          out.groups += 1;
          out.paths += std::min<std::uint64_t>(lanes, last - group);
        }
      }
    }
  }
  return out;
}

TEST(LaneGradient, DivergingLanesFallBackCountedAndKeepTheScalarBits) {
  Contract c = ad::testing::Plain(pricer::Product::kEuropean, pricer::OptionType::kCall, 6);
  const pricer::RunConfig base{.seed = kSeed, .paths = kRunPaths, .batches = kBatches};
  const pricer::PathShape shape{.steps = c.steps, .crossing = false};
  const auto scalar = ad::EstimateFromTable(pricer::RunBatches<ad::kGradientStats>(
      base, shape, [&] { return ScalarKernelFn(c, ad::testing::kMarket); }));
  ForEachWidth([&](auto d) {
    constexpr std::size_t kLanes = hn::MaxLanes(decltype(d)());
    const ExpectedFallbacks expected = CountSplits(base, kLanes);
    for (const std::uint32_t threads : kThreads) {
      pricer::RunConfig config = base;
      config.threads = threads;
      const LaneGradientEstimate got =
          RunLaneGradient(d, c, ad::testing::kMarket, config, DivergingKernel{});
      EXPECT_TRUE(SameGradient(got.estimate, scalar))
          << kLanes << " lanes, " << threads << " threads";
      EXPECT_EQ(got.fallback_groups, expected.groups) << kLanes << " lanes, " << threads;
      EXPECT_EQ(got.fallback_paths, expected.paths) << kLanes << " lanes, " << threads;
    }
    if (kLanes > 1) {
      EXPECT_GT(expected.groups, 0U);
    }
    std::printf(
        "measured, %zu lanes: the diverging kernel fell back on %llu groups, %llu paths of %llu; "
        "estimates the scalar tape's bits on 1, 2, 3 and 8 threads\n",
        kLanes, static_cast<unsigned long long>(expected.groups),
        static_cast<unsigned long long>(expected.paths),
        static_cast<unsigned long long>(base.paths));
  });
}

}  // namespace
}  // namespace aadp::simd
