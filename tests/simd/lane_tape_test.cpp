// The vector tape on its own, at every lane width this build can run (NOTES.md, "Vector tape:
// design and oracle", oracle items 3 and 4):
// - B16: on random straight-line programs (tests/ad/random_program.hpp) with each lane at its own
//   inputs, every lane's value and input adjoints are those of the scalar tape on the same program
//   at that lane's inputs: N scalar sweeps per program, bit for bit;
// - the tangent-adjoint dot product on lanes: each lane's <y', y-bar> from the lane tangent sweep
//   and <x', x-bar> from the lane reverse sweep are the scalar tape's bits, and within
//   tests/ad/dot_product_test.cpp's bound, computed here on the scalar tape;
// - Select's lane masks and divergence, case by case.
// A program whose lanes diverge is counted, and its values still compared; its gradient is the
// caller's to take on the scalar tape (lane_path_gradient.hpp), so it is not compared here.
#include "simd/lane_tape.hpp"

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
#include <vector>

#include "ad/bounds.hpp"
#include "ad/random_program.hpp"
#include "ad/real.hpp"  // IWYU pragma: keep (the operators Run<ad::Real> applies)
#include "ad/tape.hpp"
#include "lane_widths.hpp"
#include "simd/lane_tape_ops.hpp"

#ifndef AADP_SMOKE_SIZE
#define AADP_SMOKE_SIZE 0
#endif

namespace aadp::simd {
namespace {

namespace hn = hwy::HWY_NAMESPACE;
using ad::testing::Op;
using ad::testing::Program;
using ad::testing::Step;
using testing::ForEachWidth;
using testing::SameBits;

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kProgramSeed = 20261121;
constexpr int kPrograms = AADP_SMOKE_SIZE ? 400 : 8000;

template <class D>
LaneTapeReal<D> ApplyLanes(const Step& s, const std::vector<LaneTapeReal<D>>& v) {
  const LaneTapeReal<D>& a = v[s.a];
  const LaneTapeReal<D>& b = v[s.b];
  switch (s.op) {
    case Op::kAdd:
      return a + b;
    case Op::kSub:
      return a - b;
    case Op::kMul:
      return a * b;
    case Op::kDiv:
      return a / b;
    case Op::kNeg:
      return -a;
    case Op::kExp:
      return Exp(a);
    case Op::kLog:
      return Log(a);
    case Op::kSqrt:
      return Sqrt(a);
    case Op::kNormalCdf:
      return NormalCdf(a);
    case Op::kInverseNormal:
      return InverseNormal(a);
    case Op::kMin:
      return Min(a, b);
    case Op::kMax:
      return Max(a, b);
    case Op::kSelect:
      return Select(v[s.c] < v[s.d], a, b);
    case Op::kAddConstant:
      return a + s.constant;
    case Op::kMulConstant:
      return a * s.constant;
    case Op::kConstantMinus:
      return s.constant - a;
    case Op::kConstantOver:
      return s.constant / a;
    case Op::kSquare:
      return a * a;
    case Op::kCount:
      break;
  }
  return a;
}

// dot_product_test.cpp's bound and underflow weight, on the scalar tape.
double DotBound(double s, double w, std::size_t statements, std::size_t operands) {
  const double n = ad::bounds::RoundoffTerms(statements, operands) + 8.0;
  const double gamma = ad::bounds::Gamma(n);
  return (((2.0 * gamma * s) + ad::bounds::UnderflowTerm(w + 8.0)) * (1.0 + (2.0 * gamma))) +
         ad::bounds::kBoundRounding;
}

double UnderflowWeight(ad::Tape& tape, std::span<const ad::Real> x,
                       std::span<const double> direction, const ad::Position& end) {
  tape.ClearTangents();
  for (std::size_t k = 0; k < x.size(); ++k) tape.SetTangent(x[k], std::fabs(direction[k]));
  tape.SweepForwardAbsolute(ad::Position{});
  const ad::Tape::OperandSums sums = tape.SumOverOperands(end, ad::Position{});
  tape.ClearTangents();
  return sums.adjoints + sums.tangents;
}

// The scalar tape on one lane's inputs: its values, input adjoints, the two sides of the dot
// product and its bound.
struct ScalarRun {
  std::vector<double> values;
  std::vector<double> adjoints;
  double lhs = 0.0;
  double rhs = 0.0;
  double bound = 0.0;
};

ScalarRun RunScalar(const Program& program, std::span<const double> inputs) {
  ScalarRun out;
  ad::Tape tape;
  const ad::TapeScope scope(tape);
  std::vector<ad::Real> x;
  x.reserve(inputs.size());
  for (const double v : inputs) x.push_back(tape.Register(v));
  ad::testing::Trace<ad::Real> trace;
  ad::testing::Run<ad::Real>(program, x, trace);
  for (const ad::Real& v : trace.values) out.values.push_back(v.value());
  const ad::Position end = tape.GetPosition();
  const ad::MemoryReport memory = tape.Memory();

  for (std::size_t k = 0; k < x.size(); ++k) tape.SetTangent(x[k], program.direction[k]);
  tape.SweepForward(ad::Position{});
  for (const auto& [index, weight] : program.outputs) {
    out.lhs += weight * tape.Tangent(trace.values[index]);
  }
  tape.ClearTangents();

  for (const auto& [index, weight] : program.outputs) tape.AddAdjoint(trace.values[index], weight);
  tape.SweepBack(end, ad::Position{});
  for (std::size_t k = 0; k < x.size(); ++k) {
    out.adjoints.push_back(tape.Adjoint(x[k]));
    out.rhs += program.direction[k] * tape.Adjoint(x[k]);
  }
  tape.ClearAdjoints(end, ad::Position{});

  for (const auto& [index, weight] : program.outputs) {
    tape.AddAdjoint(trace.values[index], std::fabs(weight));
  }
  tape.SweepBackAbsolute(end, ad::Position{});
  double s = 0.0;
  for (std::size_t k = 0; k < x.size(); ++k) {
    s += std::fabs(program.direction[k]) * tape.Adjoint(x[k]);
  }
  const double w = UnderflowWeight(tape, x, program.direction, end);
  out.bound = DotBound(s, w, memory.statements, memory.operands);
  return out;
}

// Lane l's inputs: the program's, moved by l times a spread chosen per program, so masks split
// between lanes now and then. A lane that would leave the decoder's domain (at half its margins)
// keeps the program's own inputs.
std::vector<std::vector<double>> LaneInputs(const Program& program, std::size_t lanes,
                                            double spread) {
  std::vector<std::vector<double>> out(lanes, program.inputs);
  for (std::size_t lane = 1; lane < lanes; ++lane) {
    std::vector<double>& x = out[lane];
    for (std::size_t k = 0; k < x.size(); ++k) {
      const double sign = (k % 2 == 0) ? 1.0 : -1.0;
      x[k] += sign * static_cast<double>(lane) * spread * (1.0 + std::fabs(x[k]));
    }
    ad::testing::Trace<double> trace;
    ad::testing::Run<double>(program, x, trace);
    bool finite = trace.in_domain;
    for (const double v : trace.values) finite = finite && std::isfinite(v);
    if (!finite) x = program.inputs;
  }
  return out;
}

template <class D>
hn::Vec<D> Gather(const std::vector<std::vector<double>>& lanes, std::size_t k) {
  std::array<double, hn::MaxLanes(D())> values{};
  for (std::size_t lane = 0; lane < values.size(); ++lane) values[lane] = lanes[lane][k];
  return hn::LoadU(D(), values.data());
}

template <class D>
std::array<double, hn::MaxLanes(D())> Lanes(hn::Vec<D> v) {
  std::array<double, hn::MaxLanes(D())> out{};
  hn::StoreU(v, D(), out.data());
  return out;
}

struct Tally {
  std::size_t programs = 0;
  std::size_t steps = 0;
  std::size_t divergent = 0;
  std::size_t mismatches = 0;
  std::size_t lanes_checked = 0;
  double worst_ratio = 0.0;
};

// Reports the first few mismatches, and counts them all.
void Mismatch(Tally& tally, std::size_t lanes, std::size_t lane, const char* what, double got,
              double want) {
  if (tally.mismatches++ < 5) {
    ADD_FAILURE() << lanes << " lanes, lane " << lane << ": " << what << " " << got
                  << " against the scalar tape's " << want;
  }
}

template <class D>
void CheckValues(const std::vector<LaneTapeReal<D>>& values, const std::vector<ScalarRun>& scalar,
                 Tally& tally) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto lanes = Lanes<D>(values[i].vec());
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      if (!SameBits(lanes[lane], scalar[lane].values[i])) {
        Mismatch(tally, kLanes, lane, "value", lanes[lane], scalar[lane].values[i]);
      }
    }
  }
}

// The tangent sweep, then the reverse sweep, on a recorded program that did not diverge.
template <class D>
void CheckSweeps(D d, LaneTape<D>& tape, const std::vector<LaneTapeReal<D>>& values,
                 const Program& program, const std::vector<ScalarRun>& scalar, Tally& tally) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  const std::size_t n_inputs = program.inputs.size();
  const ad::Position end = tape.GetPosition();
  for (std::size_t k = 0; k < n_inputs; ++k) {
    tape.SetTangent(values[k], hn::Set(d, program.direction[k]));
  }
  tape.SweepForward(ad::Position{}, [](ad::Index /*i*/) { return 0.0; });
  hn::Vec<D> lhs = hn::Zero(d);
  for (const auto& [index, weight] : program.outputs) {
    lhs = hn::Add(lhs, hn::Mul(hn::Set(d, weight), tape.Tangent(values[index])));
  }
  for (const auto& [index, weight] : program.outputs) {
    tape.AddAdjoint(values[index], hn::Set(d, weight));
  }
  tape.SweepBack(end, ad::Position{});
  hn::Vec<D> rhs = hn::Zero(d);
  std::vector<std::array<double, kLanes>> adjoints;
  adjoints.reserve(n_inputs);
  for (std::size_t k = 0; k < n_inputs; ++k) {
    const hn::Vec<D> adjoint = tape.Adjoint(values[k]);
    adjoints.push_back(Lanes<D>(adjoint));
    rhs = hn::Add(rhs, hn::Mul(hn::Set(d, program.direction[k]), adjoint));
  }
  const auto lhs_lanes = Lanes<D>(lhs);
  const auto rhs_lanes = Lanes<D>(rhs);
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    ++tally.lanes_checked;
    const ScalarRun& want = scalar[lane];
    for (std::size_t k = 0; k < n_inputs; ++k) {
      if (!SameBits(adjoints[k][lane], want.adjoints[k])) {
        Mismatch(tally, kLanes, lane, "input adjoint", adjoints[k][lane], want.adjoints[k]);
      }
    }
    if (!SameBits(lhs_lanes[lane], want.lhs)) {
      Mismatch(tally, kLanes, lane, "<y', y-bar>", lhs_lanes[lane], want.lhs);
    }
    if (!SameBits(rhs_lanes[lane], want.rhs)) {
      Mismatch(tally, kLanes, lane, "<x', x-bar>", rhs_lanes[lane], want.rhs);
    }
    const double diff = std::fabs(lhs_lanes[lane] - rhs_lanes[lane]);
    EXPECT_LE(diff, want.bound) << kLanes << " lanes, lane " << lane << ": dot product";
    if (diff > 0.0) tally.worst_ratio = std::max(tally.worst_ratio, diff / want.bound);
  }
}

template <class D>
void CheckProgram(D d, const Program& program, double spread, Tally& tally) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  using Real = LaneTapeReal<D>;
  const auto inputs = LaneInputs(program, kLanes, spread);
  std::vector<ScalarRun> scalar;
  scalar.reserve(kLanes);
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    scalar.push_back(RunScalar(program, inputs[lane]));
  }

  LaneTape<D> tape;
  const LaneTapeScope<D> scope(tape);
  std::vector<Real> values;
  values.reserve(program.inputs.size() + program.steps.size());
  for (std::size_t k = 0; k < program.inputs.size(); ++k) {
    values.push_back(tape.Register(Gather<D>(inputs, k)));
  }
  for (const Step& s : program.steps) values.push_back(ApplyLanes<D>(s, values));
  ++tally.programs;
  tally.steps += program.steps.size();

  CheckValues<D>(values, scalar, tally);
  if (tape.Divergent()) {
    ++tally.divergent;
    return;
  }
  CheckSweeps(d, tape, values, program, scalar, tally);
}

TEST(LaneTape, LayoutAtEveryWidth) {
  ForEachWidth([](auto d) {
    using D = decltype(d);
    using Tape = LaneTape<D>;
    constexpr std::size_t kLanes = Tape::kLanes;
    std::printf(
        "layout, %zu lanes: active type %zu B, statement %zu B, operand %zu B (%.3g per lane), "
        "adjoint %zu B; scalar tape: %zu, %zu, %zu, 8\n",
        kLanes, sizeof(LaneTapeReal<D>), Tape::kStatementBytes, Tape::kOperandBytes,
        static_cast<double>(Tape::kOperandBytes) / static_cast<double>(kLanes),
        kLanes * sizeof(double), sizeof(ad::Real), ad::kStatementBytes, ad::kOperandBytes);
    EXPECT_EQ(Tape::kStatementBytes, 1U);
    EXPECT_EQ(Tape::kOperandBytes, kLanes == 1 ? 12U : 5U + (8U * kLanes));
  });
}

TEST(LaneTape, SelectSetsLaneMasksAndMarksDivergenceOnlyWhereLanesSplit) {
  ForEachWidth([](auto d) {
    using D = decltype(d);
    using Real = LaneTapeReal<D>;
    constexpr std::size_t kLanes = hn::MaxLanes(D());
    constexpr auto kAll = LaneTape<D>::kAllLanes;
    LaneTape<D> tape;
    const LaneTapeScope<D> scope(tape);
    const Real a = tape.Register(hn::Set(d, 1.0));
    const Real b = tape.Register(hn::Set(d, 2.0));
    const Real constant(3.0);
    // Lane 0 true, the rest false.
    const auto first = hn::FirstN(d, 1);
    const Real mixed_constant = Select(first, a, constant);
    EXPECT_EQ(mixed_constant.index(), a.index());
    EXPECT_EQ(mixed_constant.lanes(), 1U);
    const Real other_constant = Select(first, constant, a);
    EXPECT_EQ(other_constant.lanes(), static_cast<LaneBits>(kAll & ~1U));
    EXPECT_EQ(other_constant.active(), kLanes > 1);
    const Real same = Select(first, a, a);
    EXPECT_EQ(same.index(), a.index());
    EXPECT_EQ(same.lanes(), kAll);
    const Real uniform = Select(hn::FirstN(d, kLanes), a, b);
    EXPECT_EQ(uniform.index(), a.index());
    const Real constants = Select(first, constant, Real(4.0));
    EXPECT_FALSE(constants.active());
    EXPECT_FALSE(tape.Divergent());
    static_cast<void>(Select(first, a, b));
    EXPECT_EQ(tape.Divergent(), kLanes > 1) << kLanes << " lanes";
    // The guard's shape: a lane whose value is below the threshold loses its derivative only.
    const Real q = a * 1.0;
    const Real guarded = Select(Value(q) < 0.5, Value(q), q);
    EXPECT_EQ(guarded.lanes(), kAll);
  });
}

TEST(LaneTape, RandomProgramsMatchNScalarSweepsAndTheDotProduct) {
  ForEachWidth([](auto d) {
    constexpr std::size_t kLanes = hn::MaxLanes(decltype(d)());
    std::mt19937_64 random(kProgramSeed);
    constexpr std::array<double, 4> kSpreads = {0.0, 0x1p-20, 0x1p-6, 0.25};
    Tally tally;
    for (int draw = 0; draw < kPrograms; ++draw) {
      std::vector<std::uint8_t> bytes(8 + (5 * (1 + (random() % 48))));
      for (auto& b : bytes) b = static_cast<std::uint8_t>(random());
      const Program program = ad::testing::Decode(bytes);
      CheckProgram(d, program, kSpreads[static_cast<std::size_t>(draw) % kSpreads.size()], tally);
      if (tally.mismatches > 0) break;
    }
    std::printf(
        "measured, %zu lanes: %zu random programs, %zu steps, %zu lanes compared with a scalar "
        "sweep each, %zu programs divergent; %zu not bit-identical; dot product max |difference| "
        "/ bound %.3g\n",
        kLanes, tally.programs, tally.steps, tally.lanes_checked, tally.divergent, tally.mismatches,
        tally.worst_ratio);
    EXPECT_EQ(tally.mismatches, 0U);
    EXPECT_LT(tally.divergent, tally.programs);
    if (kLanes == 1) {
      EXPECT_EQ(tally.divergent, 0U);
    }
  });
}

}  // namespace
}  // namespace aadp::simd
