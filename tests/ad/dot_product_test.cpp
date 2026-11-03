// Check 2: the tangent-adjoint dot-product identity (EVIDENCE B20). For a program y = F(x) on the
// tape, a direction x' and a seed y-bar, the forward tangent sweep gives y' = J x' and the reverse
// sweep x-bar = J^T y-bar, and <y', y-bar> = <x', x-bar> exactly in real arithmetic.
//
// Bound, stated before measuring: both sides are sum_{j,k} y-bar_j J_jk x'_k, each J_jk a sum
// over paths of products of the same partials. By the argument of bounds.hpp each side is within
// gamma_n S of it, with S = sum_k A_k |x'_k|, A the absolute sweep seeded with |y-bar|, and
// n = 2 T + O + 8 (the 8 for the two final dot products of at most 4 terms each, plus the seed
// products). Below the normal range each product also loses up to 2^-1075 (bounds.hpp, after
// Higham, section 2.2): the forward sweep's losses reach <y', y-bar> weighted by the absolute
// adjoints seeded with |y-bar|, the reverse sweep's reach <x', x-bar> weighted by the absolute
// tangents seeded with |x'|, and the final dot products add at most 8 more. So, with W the sum
// over operands of both weights (Tape::SumOverOperands),
//   |<y', y-bar> - <x', x-bar>| <= [2 gamma_n S + 2^-1074 (W + 8)] (1 + 2 gamma_n) + 2^-1072.
// (The underflow term was first n 2^-1070; bounds.hpp says why it changed.)
// On random straight-line programs (random_program.hpp) and on whole priced paths of every
// product and treatment.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <vector>

#include "ad/gradient.hpp"
#include "ad/real.hpp"  // IWYU pragma: keep (the Real operations random_program.hpp calls)
#include "ad/tape.hpp"
#include "bounds.hpp"
#include "cases.hpp"
#include "pricer/draws.hpp"
#include "random_program.hpp"

namespace aadp::ad {
namespace {

// Development seeds, never 1729 or 271828.
constexpr std::uint64_t kProgramSeed = 20261111;
constexpr std::uint64_t kPathSeed = 20261112;
constexpr int kPrograms = 20000;
constexpr std::uint64_t kPathsPerCase = 64;

double DotBound(double s, double w, std::size_t statements, std::size_t operands) {
  const double n = testing::RoundoffTerms(statements, operands) + 8.0;
  const double gamma = testing::Gamma(n);
  return (((2.0 * gamma * s) + testing::UnderflowTerm(w + 8.0)) * (1.0 + (2.0 * gamma))) +
         testing::kBoundRounding;
}

// After SweepBackAbsolute seeded with |y-bar|: the absolute tangents seeded with |x'|, and W.
double UnderflowWeight(Tape& tape, std::span<const Real> x, std::span<const double> direction,
                       const Position& end) {
  tape.ClearTangents();
  for (std::size_t k = 0; k < x.size(); ++k) tape.SetTangent(x[k], std::fabs(direction[k]));
  tape.SweepForwardAbsolute(Position{});
  const Tape::OperandSums sums = tape.SumOverOperands(end, Position{});
  tape.ClearTangents();
  return sums.adjoints + sums.tangents;
}

struct Worst {
  double ratio = 0.0;     // |lhs - rhs| / bound
  double relative = 0.0;  // |lhs - rhs| / S
  std::size_t checked = 0;
  void Add(double lhs, double rhs, double s, double bound) {
    checked += 1;
    const double diff = std::fabs(lhs - rhs);
    if (diff == 0.0) return;
    ratio = std::max(ratio, diff / bound);
    relative = std::max(relative, diff / s);
  }
};

TEST(DotProduct, HoldsOnRandomStraightLinePrograms) {
  std::mt19937_64 random(kProgramSeed);
  Worst worst;
  std::size_t steps = 0;
  for (int draw = 0; draw < kPrograms; ++draw) {
    std::vector<std::uint8_t> bytes(8 + (5 * (1 + (random() % 48))));
    for (auto& b : bytes) b = static_cast<std::uint8_t>(random());
    const testing::Program program = testing::Decode(bytes);
    steps += program.steps.size();

    Tape tape;
    const TapeScope scope(tape);
    std::vector<Real> x;
    x.reserve(program.inputs.size());
    for (const double v : program.inputs) x.push_back(tape.Register(v));
    testing::Trace<Real> trace;
    testing::Run<Real>(program, x, trace);
    testing::Trace<double> plain;
    testing::Run<double>(program, program.inputs, plain);
    for (std::size_t i = 0; i < plain.values.size(); ++i) {
      ASSERT_EQ(std::bit_cast<std::uint64_t>(trace.values[i].value()),
                std::bit_cast<std::uint64_t>(plain.values[i]))
          << "draw " << draw << ", value " << i;
    }
    const Position end = tape.GetPosition();
    const MemoryReport memory = tape.Memory();

    for (std::size_t k = 0; k < x.size(); ++k) tape.SetTangent(x[k], program.direction[k]);
    tape.SweepForward(Position{});
    double lhs = 0.0;
    for (const auto& [index, weight] : program.outputs) {
      lhs += weight * tape.Tangent(trace.values[index]);
    }

    for (const auto& [index, weight] : program.outputs) {
      tape.AddAdjoint(trace.values[index], weight);
    }
    tape.SweepBack(end, Position{});
    double rhs = 0.0;
    for (std::size_t k = 0; k < x.size(); ++k) rhs += program.direction[k] * tape.Adjoint(x[k]);
    tape.ClearAdjoints(end, Position{});

    for (const auto& [index, weight] : program.outputs) {
      tape.AddAdjoint(trace.values[index], std::fabs(weight));
    }
    tape.SweepBackAbsolute(end, Position{});
    double s = 0.0;
    for (std::size_t k = 0; k < x.size(); ++k) {
      s += std::fabs(program.direction[k]) * tape.Adjoint(x[k]);
    }
    const double w = UnderflowWeight(tape, x, program.direction, end);

    const double bound = DotBound(s, w, memory.statements, memory.operands);
    ASSERT_TRUE(std::isfinite(lhs) && std::isfinite(rhs)) << "draw " << draw;
    ASSERT_LE(std::fabs(lhs - rhs), bound)
        << "draw " << draw << ": <y', y-bar> " << lhs << ", <x', x-bar> " << rhs;
    worst.Add(lhs, rhs, s, bound);
  }
  std::printf(
      "measured, %d random programs, %zu steps in all: max |difference| / bound %.3g, / S "
      "%.3g\n",
      kPrograms, steps, worst.ratio, worst.relative);
}

TEST(DotProduct, HoldsOnWholePricedPathsOfEveryProduct) {
  std::mt19937_64 random(kPathSeed);
  std::uniform_real_distribution<double> unit(-1.0, 1.0);
  for (const std::uint32_t steps : {1U, 12U, 50U}) {
    Worst worst;
    for (const testing::Case& c : testing::AllCases(steps)) {
      PathGradient path(c.contract, testing::kMarket);
      Tape& tape = path.tape();
      std::vector<double> normal(steps);
      std::vector<double> crossing(steps);
      const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
      for (std::uint64_t p = 0; p < kPathsPerCase; ++p) {
        pricer::FillPath(kPathSeed, p, normal, crossing);
        std::array<double, kInputs> direction{};
        for (double& d : direction) d = unit(random);
        const double seed = unit(random);

        const Real payoff = path.Record(u);
        const Position end = tape.GetPosition();
        const MemoryReport memory = tape.Memory();
        for (std::size_t k = 0; k < kInputs; ++k) tape.SetTangent(path.inputs()[k], direction[k]);
        tape.SweepForward(Position{});
        const double lhs = seed * tape.Tangent(payoff);

        tape.AddAdjoint(payoff, seed);
        tape.SweepBack(end, Position{});
        double rhs = 0.0;
        for (std::size_t k = 0; k < kInputs; ++k) {
          rhs += direction[k] * tape.Adjoint(path.inputs()[k]);
        }
        tape.ClearAdjoints(end, Position{});

        tape.AddAdjoint(payoff, std::fabs(seed));
        tape.SweepBackAbsolute(end, Position{});
        double s = 0.0;
        for (std::size_t k = 0; k < kInputs; ++k) {
          s += std::fabs(direction[k]) * tape.Adjoint(path.inputs()[k]);
        }
        const double w = UnderflowWeight(tape, path.inputs(), direction, end);
        tape.ClearAdjoints(end, Position{});
        path.Rewind();

        const double bound = DotBound(s, w, memory.statements, memory.operands);
        ASSERT_LE(std::fabs(lhs - rhs), bound)
            << c.name << ", " << steps << " steps, path " << p << ": <y', y-bar> " << lhs
            << ", <x', x-bar> " << rhs;
        worst.Add(lhs, rhs, s, bound);
      }
    }
    std::printf(
        "measured, every product and treatment at %u steps, %zu paths: max |difference| / bound "
        "%.3g, / S %.3g\n",
        steps, worst.checked, worst.ratio, worst.relative);
  }
}

}  // namespace
}  // namespace aadp::ad
