// The surface and local-variance entry point, and short local-volatility paths on the tape, from
// raw bytes (NOTES.md, "Stage 7a"). 272 bytes: the low bit of byte 1 picks scaled mode, in which
// 16-bit fields map onto synthetic parameter ranges that mostly validate, or raw mode, in which
// the slices' numbers are raw doubles, to drive validation and the floor rule's edges.
//
// Properties:
// - ValidateSurface and ValidateLocalVol return errors as values, and PriceLocalVol refuses
//   exactly what ValidateLocalVol refuses;
// - at a decoded (k, t): the local variance is finite and in [floor, cap], its class is Classify's
//   on its own values, an admissible point is dT w / g bit for bit, a floored one the floor and a
//   capped one the cap;
// - on the tape the same point has the double's bits, and the gradient of w + v in every
//   parameter is finite, floored or not;
// - on a valid contract, per path: the tape's value is the double kernel's bit for bit, its
//   gradient is finite and agrees with the forward-mode Dual on the seeded slice within
//   AgreementBound (src/ad/bounds.hpp);
// - the batch run with the tape has PriceLocalVol's value bits and tally, and finite Greeks, and
//   the tally adds up to the path-steps.
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "ad/bounds.hpp"
#include "ad/local_vol_gradient.hpp"
#include "ad/real.hpp"
#include "ad/tape.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/dual_ops.hpp"  // IWYU pragma: keep (tests/pricer/: the Dual overloads, by ADL)
#include "pricer/local_vol.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/runner.hpp"
#include "pricer/substep_kernels.hpp"
#include "support/dual.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

#define AADP_FUZZ_REQUIRE(condition, what)                                                   \
  do {                                                                                       \
    if (!(condition)) {                                                                      \
      std::fprintf(stderr, "invariant failed: %s: %s (%s:%d)\n", what, #condition, __FILE__, \
                   __LINE__);                                                                \
      std::abort();                                                                          \
    }                                                                                        \
  } while (false)

namespace {

using namespace aadp;
using pricer::Contract;

constexpr std::size_t kInputBytes = 272;
constexpr std::size_t kRawOffset = 64;
constexpr std::uint64_t kPaths = 8;
constexpr std::uint64_t kCheckedPaths = 2;

template <class T>
T Read(const std::uint8_t* data, std::size_t offset) {
  T value;
  std::memcpy(&value, data + offset, sizeof(T));
  return value;
}

// A 16-bit field as a fraction in [0, 1].
double Unit(const std::uint8_t* data, std::size_t offset) {
  return static_cast<double>(Read<std::uint16_t>(data, offset)) / 65535.0;
}
double Lerp(double lo, double hi, double u) { return lo + (u * (hi - lo)); }
double LogLerp(double lo, double hi, double u) {
  return std::exp(Lerp(std::log(lo), std::log(hi), u));
}

struct Input {
  std::vector<svi::SliceTerms> terms;
  std::vector<svi::RawSvi<double>> params;
  svi::LocalVolRule rule;
  double k = 0.0;
  double t_fraction = 0.0;
  pricer::LocalVolContract contract;
  std::size_t seeded = 0;
  std::uint64_t seed = 0;
};

Contract MakeContract(std::uint8_t selector, double strike, double level, std::uint32_t dates) {
  Contract c;
  c.strike = strike;
  c.steps = dates;
  c.type = (selector & 8U) != 0 ? pricer::OptionType::kPut : pricer::OptionType::kCall;
  switch (selector % 8U) {
    case 0:
      c.product = pricer::Product::kEuropean;
      return c;
    case 1:
      c.product = pricer::Product::kAsianArithmetic;
      return c;
    default:
      break;
  }
  c.product = pricer::Product::kBarrier;
  c.barrier.direction = (selector & 16U) != 0 ? pricer::Direction::kUp : pricer::Direction::kDown;
  c.barrier.level = level;
  c.barrier.smoothing_width = 0.005;
  constexpr std::array<std::pair<pricer::Monitoring, pricer::Treatment>, 6> kTreatments = {{
      {pricer::Monitoring::kDiscrete, pricer::Treatment::kOneStep},
      {pricer::Monitoring::kDiscrete, pricer::Treatment::kHard},
      {pricer::Monitoring::kDiscrete, pricer::Treatment::kSmoothed},
      {pricer::Monitoring::kContinuous, pricer::Treatment::kBridge},
      {pricer::Monitoring::kContinuous, pricer::Treatment::kHard},
      // An invalid pairing, so that refusals are fuzzed too.
      {pricer::Monitoring::kDiscrete, pricer::Treatment::kBridge},
  }};
  const auto& [monitoring, treatment] = kTreatments[(selector % 8U) - 2U];
  c.barrier.monitoring = monitoring;
  c.barrier.treatment = treatment;
  return c;
}

// Layout. Byte 0: slices - 1 (mod 3). Byte 1: bit 0 raw mode, bits 1-3 product and treatment,
// bit 4 put, bit 5 up, bits 6-7 substeps - 1 (mod 3). Byte 2: the slice the Dual seeds. Byte 3:
// dates - 1 (mod 6). Scaled slice j at 4 + 16 j: expiry gap, F, D, a, b, rho, m, sigma, each a
// uint16. 52: floor; 54: cap / floor; 56: k; 58: t / T_last; 60: K / S0; 62: H / S0; raw slice j
// at 64 + 64 j: T, F, D, a, b, rho, m, sigma as doubles; 256: maturity / T_last; 264: the seed.
Input Decode(const std::uint8_t* data) {
  Input in;
  const std::size_t slices = 1U + (data[0] % 3U);
  const bool raw = (data[1] & 1U) != 0;
  double expiry = 0.0;
  for (std::size_t j = 0; j < slices; ++j) {
    if (raw) {
      const auto field = [&](std::size_t i) {
        return Read<double>(data, kRawOffset + (64 * j) + (8 * i));
      };
      in.terms.push_back({.expiry = field(0), .forward = field(1), .discount = field(2)});
      in.params.push_back(
          {.a = field(3), .b = field(4), .rho = field(5), .m = field(6), .sigma = field(7)});
      continue;
    }
    const std::size_t o = 4 + (16 * j);
    expiry += LogLerp(1e-6, 0.5, Unit(data, o));
    in.terms.push_back({.expiry = expiry,
                        .forward = Lerp(50.0, 200.0, Unit(data, o + 2)),
                        .discount = Lerp(0.9, 1.05, Unit(data, o + 4))});
    in.params.push_back({.a = Lerp(-0.05, 0.1, Unit(data, o + 6)),
                         .b = Lerp(0.0, 3.0, Unit(data, o + 8)),
                         .rho = Lerp(-1.0, 1.0, Unit(data, o + 10)),
                         .m = Lerp(-0.5, 0.5, Unit(data, o + 12)),
                         .sigma = LogLerp(1e-8, 2.0, Unit(data, o + 14))});
  }
  const double floor = LogLerp(1e-8, 1.0, Unit(data, 52));
  in.rule = {.floor = floor,
             .cap = std::fmin(100.0, floor * (1.0 + LogLerp(1e-3, 1e4, Unit(data, 54))))};
  in.k = Lerp(-3.0, 3.0, Unit(data, 56));
  in.t_fraction = Unit(data, 58);
  in.seeded = data[2] % slices;
  in.seed = Read<std::uint64_t>(data, 264);
  const double spot = in.terms.front().forward * in.terms.front().discount;
  const double strike = spot * std::exp(Lerp(-0.3, 0.3, Unit(data, 60)));
  const double level = spot * std::exp(Lerp(-0.3, 0.3, Unit(data, 62)));
  const std::uint32_t dates = 1U + (data[3] % 6U);
  in.contract = {.contract = MakeContract(data[1] >> 1U, strike, level, dates),
                 .maturity = in.terms.back().expiry * Lerp(0.05, 1.0, Unit(data, 256)),
                 .substeps = 1U + ((data[1] >> 6U) % 3U)};
  return in;
}

bool SameBits(double a, double b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

void CheckPoint(const Input& in) {
  const svi::SviSurface<double> surface(in.terms, in.params);
  const double t = in.t_fraction * surface.Curve().LastExpiry();
  const svi::TimeSlot slot = surface.Locate(t);
  const svi::SurfacePoint<double> p = surface.At(slot, in.k);
  const svi::LocalVariance<double> lv = svi::LocalVarianceAt(p, in.k, in.rule);
  AADP_FUZZ_REQUIRE(std::isfinite(lv.variance), "local variance not finite");
  AADP_FUZZ_REQUIRE(lv.variance >= in.rule.floor && lv.variance <= in.rule.cap,
                    "local variance outside [floor, cap]");
  AADP_FUZZ_REQUIRE(lv.cls == svi::Classify(p.q_den, p.dwdt, lv.g, in.rule),
                    "class is not Classify's on the point's own values");
  if (lv.cls == svi::LocalVarianceClass::kOk) {
    AADP_FUZZ_REQUIRE(SameBits(lv.variance, p.dwdt / lv.g), "admissible point is not dT w / g");
  } else if (lv.cls == svi::LocalVarianceClass::kCapped) {
    AADP_FUZZ_REQUIRE(lv.variance == in.rule.cap, "capped point is not the cap");
  } else {
    AADP_FUZZ_REQUIRE(lv.variance == in.rule.floor, "floored point is not the floor");
  }

  ad::Tape tape;
  const ad::TapeScope scope(tape);
  std::vector<ad::Real> inputs;
  std::vector<svi::RawSvi<ad::Real>> active;
  for (const svi::RawSvi<double>& q : in.params) {
    for (const double x : {q.a, q.b, q.rho, q.m, q.sigma}) inputs.push_back(tape.Register(x));
    const std::size_t i = inputs.size() - 5;
    active.push_back({.a = inputs[i],
                      .b = inputs[i + 1],
                      .rho = inputs[i + 2],
                      .m = inputs[i + 3],
                      .sigma = inputs[i + 4]});
  }
  const svi::SviSurface<ad::Real> on_tape(in.terms, active);
  const svi::SurfacePoint<ad::Real> q = on_tape.At(slot, ad::Real(in.k));
  const svi::LocalVariance<ad::Real> got = svi::LocalVarianceAt(q, ad::Real(in.k), in.rule);
  AADP_FUZZ_REQUIRE(got.cls == lv.cls && SameBits(got.variance.value(), lv.variance),
                    "tape local variance differs from the double's");
  tape.AddAdjoint(q.w + got.variance, 1.0);
  tape.SweepBack(tape.GetPosition(), ad::Position{});
  for (const ad::Real& x : inputs) {
    AADP_FUZZ_REQUIRE(std::isfinite(tape.Adjoint(x)), "point gradient not finite");
  }
}

pricer::LocalVolModel<test::Dual> DualModel(const Input& in) {
  std::vector<svi::RawSvi<test::Dual>> params;
  for (std::size_t j = 0; j < in.params.size(); ++j) {
    const svi::RawSvi<double>& p = in.params[j];
    const auto x = [&](double v, std::size_t slot) {
      return j == in.seeded ? test::Dual::Variable(v, slot) : test::Dual(v);
    };
    params.push_back({.a = x(p.a, 0),
                      .b = x(p.b, 1),
                      .rho = x(p.rho, 2),
                      .m = x(p.m, 3),
                      .sigma = x(p.sigma, 4)});
  }
  const pricer::LocalVolContract& c = in.contract;
  return {svi::SviSurface<test::Dual>(in.terms, params), c.maturity, c.contract.steps * c.substeps,
          in.rule};
}

void CheckPaths(const Input& in) {
  const pricer::LocalVolContract& c = in.contract;
  const std::uint32_t steps = c.contract.steps * c.substeps;
  const pricer::LocalVolModel<double> model(svi::SviSurface<double>(in.terms, in.params),
                                            c.maturity, steps, in.rule);
  const pricer::LocalVolModel<test::Dual> dual_model = DualModel(in);
  ad::LocalVolPathGradient tape(c, in.terms, in.params, in.rule);
  std::vector<double> normal(steps);
  std::vector<double> crossing(steps);
  const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
  for (std::uint64_t path = 0; path < kCheckedPaths; ++path) {
    pricer::FillPath(in.seed, path, normal, crossing);
    const double value = pricer::EvaluateSubstepped<double>(c.contract, c.substeps, model, u).value;
    const test::Dual dual =
        pricer::EvaluateSubstepped<test::Dual>(c.contract, c.substeps, dual_model, u).value;
    const ad::LocalVolPathGradient::PathResult result = tape.Evaluate(u);
    AADP_FUZZ_REQUIRE(SameBits(result.value, value), "tape value differs from the double's");
    AADP_FUZZ_REQUIRE(SameBits(dual.value, value), "Dual value differs from the double's");
    for (const double g : result.gradient) {
      AADP_FUZZ_REQUIRE(std::isfinite(g), "path gradient not finite");
    }
    for (std::size_t i = 0; i < 5; ++i) {
      const std::size_t input = (5 * in.seeded) + i;
      const double bound = ad::bounds::AgreementBound(
          result.absolute[input], result.operand_sums[input], result.statements, result.operands);
      AADP_FUZZ_REQUIRE(std::fabs(result.gradient[input] - dual.tangent[i]) <= bound,
                        "tape and Dual disagree beyond the bound");
    }
  }
}

void CheckRun(const Input& in) {
  const pricer::RunConfig run{.seed = in.seed, .paths = kPaths, .batches = 2, .threads = 1};
  const auto price = pricer::PriceLocalVol(in.contract, in.terms, in.params, run, in.rule);
  const auto grad = ad::PriceLocalVolWithGradient(in.contract, in.terms, in.params, run, in.rule);
  AADP_FUZZ_REQUIRE(price.has_value() && grad.has_value(), "a valid contract was refused");
  AADP_FUZZ_REQUIRE(SameBits(price->value.mean, grad->value.mean), "tape value differs from price");
  for (const pricer::Estimate& g : grad->gradient) {
    AADP_FUZZ_REQUIRE(std::isfinite(g.mean) && std::isfinite(g.standard_error), "Greek not finite");
  }
  const pricer::FloorTally& t = price->floors;
  std::uint64_t classes = 0;
  for (const std::uint64_t n : t.by_class) classes += n;
  AADP_FUZZ_REQUIRE(classes == t.path_steps &&
                        t.path_steps == kPaths * in.contract.contract.steps * in.contract.substeps,
                    "tally does not add up to the path-steps");
  AADP_FUZZ_REQUIRE(grad->floors.by_class == t.by_class, "tape and price tallies differ");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < kInputBytes) return 0;
  const Input in = Decode(data);
  if (svi::ValidateSurface(in.terms, in.params) != svi::SurfaceError::kNone) {
    const pricer::LocalVolFailure f =
        pricer::ValidateLocalVol(in.contract, in.terms, in.params, in.rule);
    AADP_FUZZ_REQUIRE(
        f.code == pricer::LocalVolError::kSurface || f.code == pricer::LocalVolError::kRule,
        "an invalid surface was not refused");
    return 0;
  }
  CheckPoint(in);
  const pricer::LocalVolFailure f =
      pricer::ValidateLocalVol(in.contract, in.terms, in.params, in.rule);
  if (f.code != pricer::LocalVolError::kNone) {
    const pricer::RunConfig run{.seed = 1, .paths = kPaths, .batches = 2, .threads = 1};
    const auto refused = pricer::PriceLocalVol(in.contract, in.terms, in.params, run, in.rule);
    AADP_FUZZ_REQUIRE(!refused.has_value() && refused.error().code == f.code,
                      "PriceLocalVol and ValidateLocalVol disagree");
    return 0;
  }
  CheckPaths(in);
  CheckRun(in);
  return 0;
}
