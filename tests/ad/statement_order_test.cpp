// The order in which the tape records statements, pinned to one constant for every host and
// compiler.
//
// C++ leaves the order of an operator's two operands, and of a call's arguments, unspecified, and
// the compilers differ: clang evaluates left to right, g++ right to left. Code that records two
// operands in one expression therefore gives each compiler its own statement order, and the
// reverse sweep, which adds each operand's contributions in statement order, its own adjoint bits.
// That is how the clang build on the Linux host came to miss the g++ fingerprint there
// (docs/NOTES.md, "Stage 11"). The fix names such operands in clang's order.
//
// Each case below records one path on fixed synthetic inputs: GBM paths through the production
// kernels, a local-volatility path through the sub-stepped kernels and the SVI surface, and paths
// through the conditional kernels the second-order pass uses. It sweeps the tape forward from
// distinct seeds and back absolutely from the payoff, and hashes, statement by statement in
// recording order, each statement's tangent and adjoint. The values come from IEEE arithmetic and
// src/math/, which give the same bits on every host (tests/math/host_independence_test.cpp), so
// any change in the order, or in what is recorded, changes the hash on every compiler, and an
// order that follows the compiler fails on one of them.
#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "ad/real.hpp"  // IWYU pragma: keep (the Real operations the kernels call)
#include "ad/tape.hpp"
#include "pricer/conditional_kernels.hpp"
#include "pricer/contract.hpp"
#include "pricer/kernels.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/model.hpp"
#include "pricer/substep_kernels.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::ad {
namespace {

using pricer::Contract;
using pricer::Direction;
using pricer::Knock;
using pricer::Monitoring;
using pricer::OptionType;
using pricer::Product;
using pricer::Treatment;

struct Recorded {
  std::uint64_t hash = 0;
  std::size_t statements = 0;
  std::size_t operands = 0;
};

// Records one path on a fresh tape: record registers the inputs into its argument and returns
// the payoff.
Recorded RecordAndHash(const std::function<Real(Tape&, std::vector<Real>&)>& record) {
  Tape tape;
  const TapeScope scope(tape);
  std::vector<Real> inputs;
  const Real payoff = record(tape, inputs);
  EXPECT_TRUE(std::isfinite(payoff.value()));
  EXPECT_TRUE(payoff.active());
  const Position end = tape.GetPosition();
  for (std::size_t k = 0; k < inputs.size(); ++k) {
    tape.SetTangent(inputs[k], 1.0 + (0.125 * static_cast<double>(k)));
  }
  tape.SweepForward(Position{});
  tape.AddAdjoint(payoff, 1.0);
  tape.SweepBackAbsolute(end, Position{});

  Recorded out;
  out.statements = tape.Statements();
  out.operands = tape.Memory().operands;
  std::uint64_t hash = 0xcbf29ce484222325U;  // FNV-1a, 64 bits
  const auto mix = [&hash](double x) {
    const auto bits = std::bit_cast<std::uint64_t>(x);
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
      hash ^= (bits >> shift) & 0xffU;
      hash *= 0x100000001b3U;
    }
  };
  for (std::size_t i = 1; i <= out.statements; ++i) {
    mix(tape.TangentAt(static_cast<Index>(i)));
    mix(tape.AdjointAt(static_cast<Index>(i)));
  }
  out.hash = hash;
  tape.ClearAdjoints(end, Position{});
  tape.ClearTangents();
  return out;
}

pricer::Market<Real> RegisterMarket(Tape& tape, std::vector<Real>& inputs) {
  for (const double x : {100.0, 0.25, 0.03, 0.01, 0.75}) inputs.push_back(tape.Register(x));
  return {
      .spot = inputs[0], .sigma = inputs[1], .r = inputs[2], .q = inputs[3], .maturity = inputs[4]};
}

constexpr std::array<double, 4> kUniforms = {0.31, 0.62, 0.83, 0.47};

Contract MakeBarrier(OptionType type, Direction direction, Monitoring monitoring,
                     Treatment treatment, double level) {
  Contract c;
  c.product = Product::kBarrier;
  c.type = type;
  c.strike = 100.0;
  c.steps = 4;
  c.barrier = {.direction = direction,
               .knock = Knock::kOut,
               .monitoring = monitoring,
               .treatment = treatment,
               .level = level};
  return c;
}

Contract Asian(Product product, OptionType type) {
  Contract c;
  c.product = product;
  c.type = type;
  c.strike = 98.0;
  c.steps = 4;
  return c;
}

Recorded GbmPath(const Contract& c) {
  return RecordAndHash([&c](Tape& tape, std::vector<Real>& inputs) {
    const pricer::GbmModel<Real> model(RegisterMarket(tape, inputs), c.steps);
    return pricer::EvaluatePath<Real>(c, model, {.normal = kUniforms, .crossing = kUniforms}).value;
  });
}

Recorded ConditionalPath(const Contract& c) {
  return RecordAndHash([&c](Tape& tape, std::vector<Real>& inputs) {
    const pricer::GbmModel<Real> model(RegisterMarket(tape, inputs), c.steps);
    return pricer::ConditionalPayoff<Real>(c, model, {.normal = kUniforms, .crossing = {}});
  });
}

// Two slices, so the path's steps fall in both segments; every step's local variance is in the
// admissible class.
Recorded LocalVolPath(const Contract& c, std::uint32_t substeps) {
  return RecordAndHash([&c, substeps](Tape& tape, std::vector<Real>& inputs) {
    const std::array<svi::SliceTerms, 2> terms = {
        {{.expiry = 0.1, .forward = 100.0, .discount = 0.999},
         {.expiry = 0.3, .forward = 101.0, .discount = 0.997}}};
    const std::array<std::array<double, 5>, 2> raw = {
        {{0.002, 0.05, -0.3, 0.0, 0.1}, {0.006, 0.06, -0.25, 0.01, 0.12}}};
    std::vector<svi::RawSvi<Real>> slices;
    for (const auto& p : raw) {
      for (const double x : p) inputs.push_back(tape.Register(x));
      const std::size_t i = inputs.size() - 5;
      slices.push_back({.a = inputs[i],
                        .b = inputs[i + 1],
                        .rho = inputs[i + 2],
                        .m = inputs[i + 3],
                        .sigma = inputs[i + 4]});
    }
    const pricer::LocalVolModel<Real> model(svi::SviSurface<Real>(terms, slices), 0.3,
                                            c.steps * substeps, svi::LocalVolRule{});
    return pricer::EvaluateSubstepped<Real>(c, substeps, model,
                                            {.normal = kUniforms, .crossing = kUniforms})
        .value;
  });
}

struct Pin {
  const char* name = "";
  Recorded expected;
};

void ExpectPinned(const Pin& pin, const Recorded& got) {
  std::printf("%-40s statements %4zu operands %4zu hash 0x%016llx\n", pin.name, got.statements,
              got.operands, static_cast<unsigned long long>(got.hash));
  EXPECT_EQ(got.statements, pin.expected.statements) << pin.name;
  EXPECT_EQ(got.operands, pin.expected.operands) << pin.name;
  EXPECT_EQ(got.hash, pin.expected.hash) << pin.name
                                         << ": the recording order or what is recorded "
                                            "changed, or depends on the compiler";
}

TEST(StatementOrder, GbmPathsArePinned) {
  ExpectPinned({.name = "gbm down-out call, one-step survival",
                .expected = {.hash = 0x64a6c1b768ba19f6U, .statements = 62, .operands = 84}},
               GbmPath(MakeBarrier(OptionType::kCall, Direction::kDown, Monitoring::kDiscrete,
                                   Treatment::kOneStep, 90.0)));
  ExpectPinned({.name = "gbm up-out put, bridge",
                .expected = {.hash = 0xa75b5040c3aebf5dU, .statements = 70, .operands = 95}},
               GbmPath(MakeBarrier(OptionType::kPut, Direction::kUp, Monitoring::kContinuous,
                                   Treatment::kBridge, 115.0)));
}

TEST(StatementOrder, LocalVolPathIsPinned) {
  const Contract c = MakeBarrier(OptionType::kCall, Direction::kDown, Monitoring::kContinuous,
                                 Treatment::kBridge, 90.0);
  ExpectPinned({.name = "local vol down-out call, bridge, 2 substeps",
                .expected = {.hash = 0xe38fc20e3db71fe3U, .statements = 252, .operands = 388}},
               LocalVolPath(Contract{.product = c.product,
                                     .type = c.type,
                                     .strike = c.strike,
                                     .steps = 2,
                                     .barrier = c.barrier},
                            2));
}

TEST(StatementOrder, ConditionalPathsArePinned) {
  ExpectPinned({.name = "conditional arithmetic Asian call",
                .expected = {.hash = 0x84deff6bcd325dfdU, .statements = 51, .operands = 71}},
               ConditionalPath(Asian(Product::kAsianArithmetic, OptionType::kCall)));
  ExpectPinned({.name = "conditional arithmetic Asian put",
                .expected = {.hash = 0x2cf872d1994b8a90U, .statements = 52, .operands = 71}},
               ConditionalPath(Asian(Product::kAsianArithmetic, OptionType::kPut)));
  ExpectPinned({.name = "conditional geometric Asian call",
                .expected = {.hash = 0x2b3b20af824298ddU, .statements = 47, .operands = 65}},
               ConditionalPath(Asian(Product::kAsianGeometric, OptionType::kCall)));
  ExpectPinned({.name = "conditional up-out call, bridge",
                .expected = {.hash = 0xa1821398272861b9U, .statements = 132, .operands = 189}},
               ConditionalPath(MakeBarrier(OptionType::kCall, Direction::kUp,
                                           Monitoring::kContinuous, Treatment::kBridge, 115.0)));
  ExpectPinned({.name = "conditional down-out put, one-step",
                .expected = {.hash = 0x757576171c3ace39U, .statements = 75, .operands = 103}},
               ConditionalPath(MakeBarrier(OptionType::kPut, Direction::kDown,
                                           Monitoring::kDiscrete, Treatment::kOneStep, 90.0)));
}

}  // namespace
}  // namespace aadp::ad
