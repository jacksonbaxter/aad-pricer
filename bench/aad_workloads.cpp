#include "aad_workloads.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aad_steps.hpp"
#include "ad/gradient.hpp"
#include "ad/hand_adjoint.hpp"
#include "ad/second_order.hpp"
#include "ad/tape.hpp"
#include "lax_wendroff.hpp"
#include "oracle/tolerance.hpp"
#include "platform/cpu_info.hpp"
#include "pricer/bump.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "simd/lane_gradient.hpp"
#include "simd/simd_pricer.hpp"

namespace aadp::bench::aad {

namespace {

using pricer::Contract;
using pricer::Market;
using pricer::RunConfig;

constexpr std::array<std::string_view, kVariants.size()> kVariantNames = {
    "price",        "tape",         "tape-skip",  "tape-record", "hand",
    "bump-central", "bump-forward", "cond-price", "cond-tape",   "for-spot",
    "for-spot-vol", "price-lanes",  "tape-lanes"};

Contract Barrier() {
  Contract c;
  c.product = pricer::Product::kBarrier;
  c.type = pricer::OptionType::kCall;
  c.strike = 120.0;
  c.steps = 156;
  // Savine's smoothing of 5 on a spot of 100: a half-width of 0.05 S0.
  c.barrier = {.direction = pricer::Direction::kUp,
               .knock = pricer::Knock::kOut,
               .monitoring = pricer::Monitoring::kDiscrete,
               .treatment = pricer::Treatment::kSmoothed,
               .level = 150.0,
               .smoothing_width = 0.05};
  return c;
}

// The AAD-1 contract under one-step survival, which has second derivatives, in place of the
// smoothing, which is biased by design (pricer::ValidateConditional refuses it).
Contract OneStepBarrier() {
  Contract c = Barrier();
  c.barrier.treatment = pricer::Treatment::kOneStep;
  c.barrier.smoothing_width = 0.0;
  return c;
}

Contract Asian(std::uint32_t fixings) {
  Contract c;
  c.product = pricer::Product::kAsianArithmetic;
  c.type = pricer::OptionType::kCall;
  c.strike = 100.0;
  c.steps = fixings;
  return c;
}

constexpr Market<double> kBarrierMarket{
    .spot = 100.0, .sigma = 0.2, .r = 0.03, .q = 0.0, .maturity = 3.0};
// NablaTensor's Asian: K = 100, vol 20%, r 3%, T = 1 (BENCHMARKS.md MC-1).
constexpr Market<double> kAsianMarket{
    .spot = 100.0, .sigma = 0.2, .r = 0.03, .q = 0.0, .maturity = 1.0};

constexpr std::string_view kBarrierDifferences =
    "GBM at a flat 20% vol, not Dupire local vol on 60 times by 30 spots (stage 7); Philox "
    "pseudo-random draws, not Sobol; 5 inputs, not 1,801 sensitivities (1,081 active); r 3% and "
    "q 0, which the source does not state; a discrete barrier at the 156 dates with Savine's "
    "linear ramp of half-width 5, as the source";

constexpr std::string_view kAsianDifferences =
    "one spot, not a ladder of seven; Philox draws generated inline, not cached (the source's "
    "0.197 million paths/s is cached, 0.127 fresh); the five Greeks are delta, vega, rho, "
    "dividend rho and theta, not delta, strike, vega, rho and maturity; q 0, which the source "
    "does not state";

std::vector<Workload> MakeWorkloads() {
  std::vector<Workload> out;
  out.push_back({.id = "aad1-barrier",
                 .targets = "AAD-1",
                 .description = "up-and-out call, S0 100, K 120, H 150, discrete at 156 weekly "
                                "dates, smoothed (half-width 5), 3 years, 500,000 paths",
                 .differences = kBarrierDifferences,
                 .kind = Kind::kPricer,
                 .contract = Barrier(),
                 .market = kBarrierMarket,
                 .paths = 500'000,
                 .threaded = false,
                 .hand = false,
                 .study = "aad1-barrier",
                 .central = kAad1Central,
                 .forward = kAad1Forward});
  out.push_back({.id = "aad3-barrier-mt",
                 .targets = "AAD-3",
                 .description = "the AAD-1 workload on --aadp-threads worker threads",
                 .differences = kBarrierDifferences,
                 .kind = Kind::kPricer,
                 .contract = Barrier(),
                 .market = kBarrierMarket,
                 .paths = 500'000,
                 .threaded = true,
                 .hand = false,
                 .study = "aad1-barrier",
                 .central = kAad1Central,
                 .forward = kAad1Forward});
  out.push_back({.id = "aad4-asian12",
                 .targets = "AAD-4, AAD-5, AAD-6",
                 .description = "arithmetic Asian call, S0 = K = 100, 12 fixings, 1 year, "
                                "300,000 paths",
                 .differences = "one asset on GBM, not a best-of on up to 20 assets in local "
                                "vol; a full tape of the path, not Capriotti's Tapenade payout "
                                "adjoint with a separate state tangent; delta, vega, rho, "
                                "dividend rho and theta, not delta and vega alone",
                 .kind = Kind::kPricer,
                 .contract = Asian(12),
                 .market = kAsianMarket,
                 .paths = 300'000,
                 .threaded = false,
                 .hand = true,
                 .study = "aad4-asian12",
                 .central = kAsian12Central,
                 .forward = kAsian12Forward});
  out.push_back({.id = "mc-asian252",
                 .targets = "MC-1, MC-2, AAD-4 at 252 fixings, AAD-5, AAD-6",
                 .description = "arithmetic Asian call, S0 = K = 100, 252 fixings, 1 year, "
                                "300,000 paths",
                 .differences = kAsianDifferences,
                 .kind = Kind::kPricer,
                 .contract = Asian(252),
                 .market = kAsianMarket,
                 .paths = 300'000,
                 .threaded = false,
                 .hand = true,
                 .study = "mc-asian252",
                 .central = kAsian252Central,
                 .forward = kAsian252Forward});
  out.push_back({.id = "mc3-asian252-mt",
                 .targets = "MC-3",
                 .description = "the MC-1 workload on --aadp-threads worker threads",
                 .differences = kAsianDifferences,
                 .kind = Kind::kPricer,
                 .contract = Asian(252),
                 .market = kAsianMarket,
                 .paths = 300'000,
                 .threaded = true,
                 .hand = true,
                 .study = "mc-asian252",
                 .central = kAsian252Central,
                 .forward = kAsian252Forward});
  // Second order (proposed AAD-8): their multiples are reported apart, never in AAD-1 or AAD-4.
  out.push_back({.id = "aad8-asian12",
                 .targets = "AAD-8 (proposed): second order on AAD-4's contract",
                 .description = "arithmetic Asian call, S0 = K = 100, 12 fixings, 1 year, "
                                "300,000 paths; gamma, vanna and volga by forward over reverse "
                                "on the conditional kernels",
                 .differences = "AAD-4's contract and paths; the second-order and cond-* "
                                "variants price the last step's expectation in closed form, so "
                                "their primal is cond-price, not price",
                 .kind = Kind::kPricer,
                 .contract = Asian(12),
                 .market = kAsianMarket,
                 .paths = 300'000,
                 .threaded = false,
                 .hand = false,
                 .second_order = true,
                 .study = "aad4-asian12",
                 .central = kAsian12Central,
                 .forward = kAsian12Forward});
  out.push_back({.id = "aad8-barrier",
                 .targets = "AAD-8 (proposed): second order on AAD-1's contract",
                 .description = "up-and-out call, S0 100, K 120, H 150, discrete at 156 weekly "
                                "dates, one-step survival, 3 years, 500,000 paths; gamma, vanna "
                                "and volga by forward over reverse on the conditional kernels",
                 .differences = "AAD-1's contract with one-step survival (Gerstner, Harrach, "
                                "Roth, arXiv 1804.03975v3) in place of Savine's smoothing, which "
                                "has no unbiased second derivative; price and tape run the "
                                "production one-step kernel",
                 .kind = Kind::kPricer,
                 .contract = OneStepBarrier(),
                 .market = kBarrierMarket,
                 .paths = 500'000,
                 .threaded = false,
                 .hand = false,
                 .second_order = true,
                 .study = "aad1-barrier",
                 .central = kAad1Central,
                 .forward = kAad1Forward});
  out.push_back({.id = "lw-worst",
                 .targets = "B34 (Hogan's LW: Adept 32 to 33 times, hand 2.1)",
                 .description = "Lax-Wendroff advection, 100 points, 2000 steps, c = 0.5, "
                                "gradient of J = w . q by 100 inputs",
                 .differences = "c = 0.5, which Hogan does not state; the 100 output adjoints "
                                "are seeded with a ramp w through J = w . q",
                 .kind = Kind::kLaxWendroff,
                 .contract = {},
                 .market = {},
                 .paths = 1,
                 .threaded = false,
                 .hand = true,
                 .study = "lw-worst",
                 .central = kLwCentral,
                 .forward = kLwForward});
  return out;
}

void Require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}

VariantResult FromGradient(Variant variant, ad::GradientEstimate estimate) {
  VariantResult out;
  out.variant = variant;
  out.value = std::move(estimate.value);
  out.greeks = std::move(estimate.greeks);
  out.has_greeks = true;
  return out;
}

template <std::size_t N>
VariantResult FromHessian(Variant variant, ad::HessianEstimate<N> estimate) {
  VariantResult out;
  out.variant = variant;
  out.value = std::move(estimate.value);
  out.greeks = std::move(estimate.greeks);
  out.has_greeks = true;
  for (auto& row : estimate.rows) out.hessian_rows.push_back(std::move(row));
  return out;
}

// The tape with the zero-adjoint skip: GradientPathFn's steps (src/ad/gradient.hpp), with
// SweepBackSkippingZeros for the path's sweep. The set-up sweep is the plain one.
class SkipPathFn {
 public:
  SkipPathFn(const Contract& contract, const Market<double>& market) : path_(contract, market) {}

  void operator()(const pricer::PathUniforms& u, std::array<double, ad::kGradientStats>& stats) {
    stats = {};
    const ad::Real payoff = path_.Record(u);
    ad::Tape& tape = path_.tape();
    tape.AddAdjoint(payoff, 1.0);
    tape.SweepBackSkippingZeros(tape.GetPosition(), path_.mark());
    path_.Rewind();
    stats[0] = payoff.value();
  }
  void EndBlock(std::array<double, ad::kGradientStats>& sum) {
    const std::array<double, ad::kInputs> gradient = path_.SweepSetup();
    for (std::size_t i = 0; i < ad::kInputs; ++i) {
      sum[1 + i] += i == ad::kMaturityInput ? -gradient[i] : gradient[i];
    }
  }

 private:
  ad::PathGradient path_;
};

// The tape's record half: each path recorded and rewound, never swept.
class RecordPathFn {
 public:
  RecordPathFn(const Contract& contract, const Market<double>& market) : path_(contract, market) {}

  void operator()(const pricer::PathUniforms& u, std::array<double, 1>& stats) {
    const ad::Real payoff = path_.Record(u);
    path_.Rewind();
    stats[0] = payoff.value();
  }

 private:
  ad::PathGradient path_;
};

pricer::PathShape ShapeOf(const Contract& c) {
  return {.steps = c.steps, .crossing = pricer::NeedsCrossingUniforms(c)};
}

pricer::Estimate ValueOf(const std::vector<std::array<double, 1>>& table) {
  std::vector<double> means;
  means.reserve(table.size());
  for (const auto& row : table) means.push_back(row[0]);
  pricer::Estimate estimate;
  const BatchStatistics stats = BatchMeansStatistics(means);
  estimate.mean = stats.mean;
  estimate.standard_error = stats.standard_error;
  estimate.batch_means = std::move(means);
  return estimate;
}

VariantResult FromBump(Variant variant, pricer::BumpEstimate bump) {
  VariantResult out;
  out.variant = variant;
  out.value = std::move(bump.value);
  out.greeks = std::move(bump.greeks);
  out.has_greeks = true;
  out.denominator.assign(bump.denominator.begin(), bump.denominator.end());
  out.absolute_up.assign(bump.mean_absolute_up.begin(), bump.mean_absolute_up.end());
  out.absolute_down.assign(bump.mean_absolute_down.begin(), bump.mean_absolute_down.end());
  return out;
}

Runner PricerRunner(const Workload& w, Variant variant, const RunConfig& config) {
  const Contract c = w.contract;
  const Market<double> m = w.market;
  switch (variant) {
    case Variant::kPrice:
      return [c, m, config] {
        auto price = pricer::Price(c, m, config);
        Require(price.has_value(), "price refused");
        VariantResult out;
        out.value = std::move(*price);
        return out;
      };
    case Variant::kTape:
      return [c, m, config] {
        auto run = ad::PriceWithGradient(c, m, config);
        Require(run.has_value(), "tape refused");
        return FromGradient(Variant::kTape, std::move(*run));
      };
    case Variant::kTapeSkip:
      return [c, m, config] {
        return FromGradient(Variant::kTapeSkip,
                            ad::EstimateFromTable(pricer::RunBatches<ad::kGradientStats>(
                                config, ShapeOf(c), [&] { return SkipPathFn(c, m); })));
      };
    case Variant::kTapeRecord:
      return [c, m, config] {
        VariantResult out;
        out.variant = Variant::kTapeRecord;
        out.value =
            ValueOf(pricer::RunBatches<1>(config, ShapeOf(c), [&] { return RecordPathFn(c, m); }));
        return out;
      };
    case Variant::kHand:
      return [c, m, config] {
        auto run = ad::hand::PriceWithHandAdjoint(c, m, config);
        Require(run.has_value(), "hand adjoint refused");
        return FromGradient(Variant::kHand, std::move(*run));
      };
    case Variant::kBumpCentral:
    case Variant::kBumpForward: {
      const pricer::BumpScheme scheme = variant == Variant::kBumpCentral
                                            ? pricer::BumpScheme::kCentral
                                            : pricer::BumpScheme::kForward;
      const pricer::BumpSteps steps = Steps(w, scheme);
      return [c, m, config, scheme, steps, variant] {
        auto run = pricer::PriceWithBumps(c, m, config, scheme, steps);
        Require(run.has_value(), "bump refused");
        return FromBump(variant, std::move(*run));
      };
    }
    case Variant::kCondPrice:
      return [c, m, config] {
        auto run = ad::PriceConditional(c, m, config);
        Require(run.has_value(), "conditional price refused");
        VariantResult out;
        out.variant = Variant::kCondPrice;
        out.value = std::move(*run);
        return out;
      };
    case Variant::kCondTape:
      return [c, m, config] {
        auto run = ad::PriceWithConditionalGradient(c, m, config);
        Require(run.has_value(), "conditional tape refused");
        return FromGradient(Variant::kCondTape, std::move(*run));
      };
    case Variant::kForSpot:
      return [c, m, config] {
        auto run = ad::PriceWithHessian<1>(c, m, config, {ad::kSpotInput});
        Require(run.has_value(), "forward over reverse refused");
        return FromHessian(Variant::kForSpot, std::move(*run));
      };
    case Variant::kForSpotVol:
      return [c, m, config] {
        auto run = ad::PriceWithHessian<2>(c, m, config, {ad::kSpotInput, ad::kVolInput});
        Require(run.has_value(), "forward over reverse refused");
        return FromHessian(Variant::kForSpotVol, std::move(*run));
      };
    case Variant::kPriceLanes:
      return [c, m, config, lanes = LaneCount()] {
        auto run = simd::PriceLanes(c, m, config, lanes);
        Require(run.has_value(), "lane primal refused");
        VariantResult out;
        out.variant = Variant::kPriceLanes;
        out.value = std::move(*run);
        return out;
      };
    case Variant::kTapeLanes:
      return [c, m, config, lanes = LaneCount()] {
        auto run = simd::PriceWithGradientLanes(c, m, config, lanes);
        Require(run.has_value(), "vector tape refused");
        VariantResult out = FromGradient(Variant::kTapeLanes, std::move(run->estimate));
        out.fallback_groups = run->fallback_groups;
        out.fallback_paths = run->fallback_paths;
        return out;
      };
  }
  throw std::runtime_error("unknown variant");
}

// One Lax-Wendroff run on a tape kept across runs, so that after the first run nothing is
// allocated (Hogan: "memory reuse"). The scope is per run, because several runners share a thread.
class LwTapeRunner {
 public:
  explicit LwTapeRunner(Variant variant) : variant_(variant), tape_(std::make_shared<ad::Tape>()) {}

  VariantResult operator()() const {
    ad::Tape& tape = *tape_;
    const ad::TapeScope scope(tape);
    const ad::Position start = tape.GetPosition();
    lw::Field<ad::Real> q;
    for (std::size_t i = 0; i < lw::kPoints; ++i) q[i] = tape.Register(q0_[i]);
    const lw::Field<ad::Real> inputs = q;
    lw::Advect(q, lw::kTimeSteps, lw::kCourant);
    const ad::Real j = lw::Functional(q, w_);
    VariantResult out;
    out.variant = variant_;
    out.value.mean = j.value();
    if (variant_ != Variant::kTapeRecord) {
      const ad::Position end = tape.GetPosition();
      tape.AddAdjoint(j, 1.0);
      if (variant_ == Variant::kTapeSkip) {
        tape.SweepBackSkippingZeros(end, start);
      } else {
        tape.SweepBack(end, start);
      }
      for (std::size_t i = 0; i < lw::kPoints; ++i) out.gradient[i] = tape.Adjoint(inputs[i]);
      // The sweep zeroes every adjoint it propagates, so only the inputs keep one.
      ad::Position inputs_end = start;
      inputs_end.statements += lw::kPoints;
      tape.ClearAdjoints(inputs_end, start);
    }
    tape.Rewind(start);
    return out;
  }

 private:
  Variant variant_;
  std::shared_ptr<ad::Tape> tape_;
  lw::Field<double> q0_ = lw::InitialField();
  lw::Field<double> w_ = lw::OutputWeights();
};

VariantResult LwBumps(Variant variant, double h) {
  const lw::Field<double> q0 = lw::InitialField();
  const lw::Field<double> w = lw::OutputWeights();
  VariantResult out;
  out.variant = variant;
  out.value.mean = lw::Primal(q0, w);
  out.denominator.resize(lw::kPoints);
  out.absolute_up.resize(lw::kPoints);
  out.absolute_down.resize(lw::kPoints);
  for (std::size_t k = 0; k < lw::kPoints; ++k) {
    lw::Field<double> up = q0;
    up[k] += h;
    const double j_up = lw::Primal(up, w);
    double j_down = out.value.mean;
    double x_down = q0[k];
    if (variant == Variant::kBumpCentral) {
      lw::Field<double> down = q0;
      down[k] -= h;
      j_down = lw::Primal(down, w);
      x_down = down[k];
    }
    out.denominator[k] = up[k] - x_down;
    out.gradient[k] = (j_up - j_down) / out.denominator[k];
    out.absolute_up[k] = std::fabs(j_up);
    out.absolute_down[k] = std::fabs(j_down);
  }
  return out;
}

Runner LwRunner(const Workload& w, Variant variant) {
  switch (variant) {
    case Variant::kPrice:
      return [] {
        VariantResult out;
        out.value.mean = lw::Primal(lw::InitialField(), lw::OutputWeights());
        return out;
      };
    case Variant::kTape:
    case Variant::kTapeSkip:
    case Variant::kTapeRecord:
      return LwTapeRunner(variant);
    case Variant::kHand:
      return [] {
        VariantResult out;
        out.variant = Variant::kHand;
        out.value.mean = lw::HandGradient(lw::InitialField(), lw::OutputWeights(), out.gradient);
        return out;
      };
    case Variant::kBumpCentral:
      return [h = w.central[0].h] { return LwBumps(Variant::kBumpCentral, h); };
    case Variant::kBumpForward:
      return [h = w.forward[0].h] { return LwBumps(Variant::kBumpForward, h); };
    case Variant::kCondPrice:
    case Variant::kCondTape:
    case Variant::kForSpot:
    case Variant::kForSpotVol:
    case Variant::kPriceLanes:
    case Variant::kTapeLanes:
      break;
  }
  throw std::runtime_error("unknown variant");
}

}  // namespace

std::string_view VariantName(Variant variant) {
  return kVariantNames[static_cast<std::size_t>(variant)];
}

std::optional<Variant> ParseVariant(std::string_view name) {
  for (std::size_t i = 0; i < kVariantNames.size(); ++i) {
    if (kVariantNames[i] == name) return kVariants[i];
  }
  return std::nullopt;
}

std::span<const Workload> Workloads() {
  static const std::vector<Workload> workloads = MakeWorkloads();
  return workloads;
}

const Workload* FindWorkload(std::string_view id) {
  for (const Workload& w : Workloads()) {
    if (w.id == id) return &w;
  }
  return nullptr;
}

bool HasVariant(const Workload& workload, Variant variant) {
  const bool second = variant == Variant::kCondPrice || variant == Variant::kCondTape ||
                      variant == Variant::kForSpot || variant == Variant::kForSpotVol;
  if (workload.second_order) {
    return second || variant == Variant::kPrice || variant == Variant::kTape;
  }
  if (variant == Variant::kPriceLanes || variant == Variant::kTapeLanes) {
    return workload.kind == Kind::kPricer;
  }
  return !second && (variant != Variant::kHand || workload.hand);
}

std::optional<RunConfig> ConfigFor(const Workload& workload, std::uint64_t divisor,
                                   std::uint32_t threads) {
  const std::uint32_t used = workload.threaded ? threads : 1;
  if (workload.kind == Kind::kLaxWendroff) {
    return RunConfig{.seed = kSeed, .paths = 1, .batches = 1, .threads = 1};
  }
  if (divisor == 0 || workload.paths % divisor != 0) return std::nullopt;
  const std::uint64_t paths = workload.paths / divisor;
  if (paths == 0 || paths % kMcBatches != 0) return std::nullopt;
  return RunConfig{.seed = kSeed, .paths = paths, .batches = kMcBatches, .threads = used};
}

pricer::BumpSteps Steps(const Workload& workload, pricer::BumpScheme scheme) {
  const StepChoices& choices =
      scheme == pricer::BumpScheme::kCentral ? workload.central : workload.forward;
  pricer::BumpSteps steps{};
  for (std::size_t i = 0; i < steps.size(); ++i) steps[i] = choices[i].h;
  return steps;
}

Runner MakeRunner(const Workload& workload, Variant variant, const RunConfig& config) {
  Require(HasVariant(workload, variant), "the workload has no such variant");
  if (workload.kind == Kind::kLaxWendroff) return LwRunner(workload, variant);
  return PricerRunner(workload, variant, config);
}

TapeShape MeasureTape(const Workload& workload, const RunConfig& config) {
  TapeShape shape;
  if (workload.kind == Kind::kLaxWendroff) {
    ad::Tape tape;
    const ad::TapeScope scope(tape);
    lw::Field<ad::Real> q;
    const lw::Field<double> q0 = lw::InitialField();
    for (std::size_t i = 0; i < lw::kPoints; ++i) q[i] = tape.Register(q0[i]);
    lw::Advect(q, lw::kTimeSteps, lw::kCourant);
    static_cast<void>(lw::Functional(q, lw::OutputWeights()));
    const ad::MemoryReport memory = tape.Memory();
    shape.statements_per_path = memory.statements;
    shape.operands_per_path = memory.operands;
    shape.mean_statements_per_path = static_cast<double>(memory.statements);
    shape.bytes_per_path = memory.statement_bytes + memory.operand_bytes;
    shape.adjoint_bytes_per_path = memory.adjoint_bytes;
    shape.allocated_bytes = memory.allocated_bytes;
    shape.paths_sampled = 1;
    return shape;
  }
  ad::PathGradient path(workload.contract, workload.market);
  const ad::MemoryReport setup = path.tape().Memory();
  shape.setup_statements = setup.statements;
  const std::uint64_t sampled = std::min<std::uint64_t>(config.paths, pricer::kBlockPaths);
  std::vector<double> normal(workload.contract.steps);
  std::vector<double> crossing(
      pricer::NeedsCrossingUniforms(workload.contract) ? workload.contract.steps : 0);
  const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
  double total = 0.0;
  for (std::uint64_t p = 0; p < sampled; ++p) {
    pricer::FillPath(config.seed, p, normal, crossing);
    static_cast<void>(path.Record(u));
    const ad::MemoryReport memory = path.tape().Memory();
    const std::size_t statements = memory.statements - setup.statements;
    const std::size_t operands = memory.operands - setup.operands;
    total += static_cast<double>(statements);
    if (statements > shape.statements_per_path) {
      shape.statements_per_path = statements;
      shape.operands_per_path = operands;
    }
    path.Rewind();
  }
  shape.mean_statements_per_path = total / static_cast<double>(sampled);
  shape.bytes_per_path = (shape.statements_per_path * ad::kStatementBytes) +
                         (shape.operands_per_path * ad::kOperandBytes);
  shape.adjoint_bytes_per_path = shape.statements_per_path * sizeof(double);
  shape.allocated_bytes = path.tape().Memory().allocated_bytes;
  shape.paths_sampled = static_cast<std::size_t>(sampled);
  return shape;
}

LaneTapeShape MeasureLaneTape(const Workload& workload, const RunConfig& config) {
  Require(HasVariant(workload, Variant::kTapeLanes), "the workload has no vector tape");
  const auto memory =
      simd::MeasureLaneTape(workload.contract, workload.market, config, LaneCount());
  Require(memory.has_value(), "the vector tape refused the workload");
  LaneTapeShape shape;
  shape.lanes = memory->lanes;
  shape.statements_per_group = memory->statements;
  shape.operands_per_group = memory->operands;
  shape.operand_bytes = memory->operand_bytes;
  const std::size_t group_bytes =
      (memory->statements * memory->statement_bytes) + (memory->operands * memory->operand_bytes);
  shape.bytes_per_path = static_cast<double>(group_bytes) / static_cast<double>(memory->lanes);
  shape.allocated_bytes = memory->allocated_bytes;
  shape.groups_sampled = memory->groups;
  return shape;
}

std::size_t LaneCount() { return simd::MaxLanes(); }

std::string LaneTarget() { return simd::TargetName(); }

std::string LaneCeiling() {
  const std::string lanes = std::to_string(LaneCount());
  // A ceiling on the double arithmetic per instruction. Work the lanes share (a group's indices,
  // operand counts and tape lookups) is done once per group, so a tape speedup can pass it.
  std::string out = lanes + " double lanes on " + LaneTarget() + ", at most " + lanes +
                    " doubles of arithmetic per instruction";
  const std::string datapath(platform::fp_datapath_note(platform::read_x86_model()));
  if (!datapath.empty()) {
    out += "; " + datapath;
    // 128 bits hold 2 doubles: a wider vector runs as 128-bit halves there.
    if (LaneCount() > 2) out += ", which runs each 256-bit operation as two 128-bit halves";
  }
  return out;
}

double NonzeroPayoffFraction(const Workload& workload, const RunConfig& config) {
  if (workload.kind == Kind::kLaxWendroff) return 1.0;
  const Contract c = workload.contract;
  const pricer::GbmModel<double> model(workload.market, c.steps);
  const auto table = pricer::RunBatches<1>(config, ShapeOf(c), [&] {
    return [&](const pricer::PathUniforms& u, std::array<double, 1>& out) {
      out[0] = pricer::DiscountedPayoff<double>(c, model, u) != 0.0 ? 1.0 : 0.0;
    };
  });
  double sum = 0.0;
  for (const auto& row : table) sum += row[0];
  return sum / static_cast<double>(table.size());
}

}  // namespace aadp::bench::aad
