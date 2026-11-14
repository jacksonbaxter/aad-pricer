// Unit tests of the tape itself: every operation's partial in reverse and tangent mode
// (EVIDENCE B18), passive constants, Select, chunk boundaries, mark and rewind, the set-up sweep,
// and activation (B10).
#include "ad/tape.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ad/real.hpp"
#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "pricer/normal_cdf.hpp"

namespace aadp::ad {
namespace {

// The partials are formulas on src/math/ values, so against the same formulas on libm they may
// differ by a few ULP of each function: 1e-14 relative covers that with room, and a wrong partial
// is wrong by far more.
constexpr double kAnalyticRelative = 1e-14;

double Relative(double a, double b) { return std::fabs(a - b) / std::fmax(std::fabs(b), 1e-300); }

struct UnaryCase {
  std::string name;
  std::function<Real(const Real&)> f;
  std::function<double(double)> derivative;  // On libm, written independently.
  std::vector<double> points;
  // The partial 1 / phi(z) of the inverse normal carries AS241's relative error e in z as
  // z^2 e relative (d log phi / dz = -z), e below 1e-15 (stage 1a), so its allowance grows with
  // z^2. Every other partial gets kAnalyticRelative.
  bool scales_with_z_squared = false;
};

double LibmPdf(double x) { return std::exp(-0.5 * x * x) / std::sqrt(2.0 * std::numbers::pi); }

// z with Phi(z) = p, by Newton on libm's erfc in the tail where 1 - p or p is exact, so the
// reference is well conditioned on both sides.
double LibmInverseNormal(double p) {
  const bool upper = p > 0.5;
  const double tail = upper ? 1.0 - p : p;
  double z = -1.0;
  for (int i = 0; i < 100; ++i) {
    z -= (0.5 * std::erfc(-z / std::numbers::sqrt2) - tail) / LibmPdf(z);
  }
  return upper ? -z : z;
}

std::vector<UnaryCase> UnaryCases() {
  return {
      {.name = "negate",
       .f = [](const Real& x) { return -x; },
       .derivative = [](double) { return -1.0; },
       .points = {-2.0, 0.5, 3.0}},
      {.name = "exp",
       .f = [](const Real& x) { return Exp(x); },
       .derivative = [](double x) { return std::exp(x); },
       .points = {-5.0, -0.3, 0.0, 1.7, 20.0}},
      {.name = "log",
       .f = [](const Real& x) { return Log(x); },
       .derivative = [](double x) { return 1.0 / x; },
       .points = {1e-8, 0.3, 1.0, 7.5, 1e6}},
      {.name = "sqrt",
       .f = [](const Real& x) { return Sqrt(x); },
       .derivative = [](double x) { return 0.5 / std::sqrt(x); },
       .points = {1e-6, 0.25, 2.0, 1e4}},
      {.name = "normal cdf",
       .f = [](const Real& x) { return NormalCdf(x); },
       .derivative = LibmPdf,
       .points = {-8.0, -1.2, 0.0, 0.4, 3.0}},
      {.name = "inverse normal",
       .f = [](const Real& x) { return InverseNormal(x); },
       .derivative = [](double p) { return 1.0 / LibmPdf(LibmInverseNormal(p)); },
       .points = {1e-6, 0.02, 0.5, 0.9, 0.999},
       .scales_with_z_squared = true},
      {.name = "plus double",
       .f = [](const Real& x) { return x + 2.5; },
       .derivative = [](double) { return 1.0; },
       .points = {-1.0, 4.0}},
      {.name = "double minus",
       .f = [](const Real& x) { return 2.5 - x; },
       .derivative = [](double) { return -1.0; },
       .points = {-1.0, 4.0}},
      {.name = "times double",
       .f = [](const Real& x) { return x * -3.0; },
       .derivative = [](double) { return -3.0; },
       .points = {0.5, 2.0}},
      {.name = "double over",
       .f = [](const Real& x) { return 3.0 / x; },
       .derivative = [](double x) { return -3.0 / (x * x); },
       .points = {-2.0, 0.5, 9.0}},
      {.name = "over double",
       .f = [](const Real& x) { return x / 4.0; },
       .derivative = [](double) { return 0.25; },
       .points = {-2.0, 9.0}},
  };
}

TEST(Tape, EveryUnaryOperationHasItsAnalyticPartialInReverseAndTangentMode) {
  for (const UnaryCase& c : UnaryCases()) {
    for (const double point : c.points) {
      Tape tape;
      const TapeScope scope(tape);
      const Real x = tape.Register(point);
      const Position start = tape.GetPosition();
      const Real y = c.f(x);
      ASSERT_TRUE(y.active()) << c.name;
      EXPECT_EQ(tape.Statements(), 2U) << c.name << ": one node per operation";
      const double expected = c.derivative(point);
      const double z = y.value();
      const double allowed = kAnalyticRelative * (c.scales_with_z_squared ? 1.0 + (z * z) : 1.0);

      tape.AddAdjoint(y, 1.0);
      tape.SweepBack(tape.GetPosition(), Position{});
      EXPECT_LE(Relative(tape.Adjoint(x), expected), allowed) << c.name << " at " << point;
      EXPECT_EQ(tape.Adjoint(y), 0.0) << c.name << ": a swept statement is zeroed";

      tape.SetTangent(x, 1.0);
      tape.SweepForward(start);
      EXPECT_LE(Relative(tape.Tangent(y), expected), allowed) << c.name << " at " << point;
    }
  }
}

struct BinaryCase {
  std::string name;
  std::function<Real(const Real&, const Real&)> f;
  std::function<double(double, double)> da;
  std::function<double(double, double)> db;
};

TEST(Tape, EveryBinaryOperationHasItsAnalyticPartialsInReverseAndTangentMode) {
  const std::vector<BinaryCase> cases = {
      {.name = "plus",
       .f = [](const Real& a, const Real& b) { return a + b; },
       .da = [](double, double) { return 1.0; },
       .db = [](double, double) { return 1.0; }},
      {.name = "minus",
       .f = [](const Real& a, const Real& b) { return a - b; },
       .da = [](double, double) { return 1.0; },
       .db = [](double, double) { return -1.0; }},
      {.name = "times",
       .f = [](const Real& a, const Real& b) { return a * b; },
       .da = [](double, double b) { return b; },
       .db = [](double a, double) { return a; }},
      {.name = "over",
       .f = [](const Real& a, const Real& b) { return a / b; },
       .da = [](double, double b) { return 1.0 / b; },
       .db = [](double a, double b) { return -a / (b * b); }},
  };
  for (const BinaryCase& c : cases) {
    for (const auto& [a0, b0] : {std::pair{1.5, -0.25}, std::pair{-3.0, 7.0}}) {
      Tape tape;
      const TapeScope scope(tape);
      const Real a = tape.Register(a0);
      const Real b = tape.Register(b0);
      const Position start = tape.GetPosition();
      const Real y = c.f(a, b);
      EXPECT_EQ(tape.Statements(), 3U) << c.name;
      tape.AddAdjoint(y, 1.0);
      tape.SweepBack(tape.GetPosition(), Position{});
      EXPECT_LE(Relative(tape.Adjoint(a), c.da(a0, b0)), kAnalyticRelative) << c.name;
      EXPECT_LE(Relative(tape.Adjoint(b), c.db(a0, b0)), kAnalyticRelative) << c.name;
      tape.SetTangent(a, 2.0);
      tape.SetTangent(b, -1.0);
      tape.SweepForward(start);
      const double expected = (2.0 * c.da(a0, b0)) - c.db(a0, b0);
      EXPECT_LE(Relative(tape.Tangent(y), expected), kAnalyticRelative) << c.name;
    }
  }
}

// The same operand twice in one statement: both partials land on it.
TEST(Tape, AnOperandUsedTwiceGetsBothPartials) {
  Tape tape;
  const TapeScope scope(tape);
  const Real x = tape.Register(3.0);
  const Real y = x * x;
  tape.AddAdjoint(y, 1.0);
  tape.SweepBack(tape.GetPosition(), Position{});
  EXPECT_EQ(tape.Adjoint(x), 6.0);
}

TEST(Tape, ValuesAreTheDoubleOperationsBitForBit) {
  Tape tape;
  const TapeScope scope(tape);
  const double v = 0.7345;
  const Real x = tape.Register(v);
  EXPECT_EQ(Exp(x).value(), math::exp(v));
  EXPECT_EQ(Log(x).value(), math::log(v));
  EXPECT_EQ(Sqrt(x).value(), math::sqrt(v));
  EXPECT_EQ(NormalCdf(x).value(), pricer::NormalCdf(v));
  EXPECT_EQ(InverseNormal(x).value(), math::inverse_normal(v));
  EXPECT_EQ((x / 3.0).value(), v / 3.0);
  EXPECT_EQ((x * x - 1.0).value(), (v * v) - 1.0);
}

TEST(Tape, PassiveConstantsRecordNothing) {
  Tape tape;
  const TapeScope scope(tape);
  const Real x = tape.Register(2.0);
  const std::size_t before = tape.Statements();
  const Real c(1.5);
  const Real d = Exp(c) * Log(c) / Sqrt(c) + NormalCdf(c) - InverseNormal(Real(0.3)) - c;
  EXPECT_FALSE(d.active());
  EXPECT_FALSE((c * 2.0).active());
  EXPECT_FALSE(Max(c, Real(4.0)).active());
  EXPECT_EQ(tape.Statements(), before);
  // A passive operand of an active one is not recorded: one operand, not two.
  const MemoryReport m0 = tape.Memory();
  const Real y = x * c;
  EXPECT_EQ(tape.Memory().operands, m0.operands + 1);
  EXPECT_TRUE(y.active());
}

TEST(Tape, SelectAliasesTheChosenOperandAndRecordsNothing) {
  Tape tape;
  const TapeScope scope(tape);
  const Real a = tape.Register(1.0);
  const Real b = tape.Register(2.0);
  const std::size_t before = tape.Statements();
  const Real s = Select(true, a, b);
  EXPECT_EQ(s.index(), a.index());
  EXPECT_EQ(Max(a, b).index(), b.index());
  EXPECT_EQ(Min(a, b).index(), a.index());
  EXPECT_EQ(tape.Statements(), before);
  const Real y = Select(a > b, a, b) * 3.0;
  tape.AddAdjoint(y, 1.0);
  tape.SweepBack(tape.GetPosition(), Position{});
  EXPECT_EQ(tape.Adjoint(a), 0.0);
  EXPECT_EQ(tape.Adjoint(b), 3.0);
}

TEST(Tape, MinAndMaxFollowStdMinAndMaxIncludingNaN) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const double a : {1.0, -2.0, nan}) {
    for (const double b : {1.0, 3.0, nan}) {
      const double max = Max(Real(a), Real(b)).value();
      const double min = Min(Real(a), Real(b)).value();
      const double std_max = std::max(a, b);
      const double std_min = std::min(a, b);
      EXPECT_TRUE(max == std_max || (std::isnan(max) && std::isnan(std_max))) << a << " " << b;
      EXPECT_TRUE(min == std_min || (std::isnan(min) && std::isnan(std_min))) << a << " " << b;
    }
  }
}

// y_{i+1} = y_i c + x over several chunks: a mix of one- and two-operand statements whose
// operand blocks must not straddle a chunk. dy_n/dx = sum_{i<n} c^i, computed in the same order.
TEST(Tape, SweepsCrossChunkBoundaries) {
  constexpr int kSteps = 3 * static_cast<int>(kChunkEntries) + 17;
  constexpr double kC = 0.999;
  Tape tape;
  const TapeScope scope(tape);
  const Real x = tape.Register(0.5);
  const Position start = tape.GetPosition();
  Real y = x;
  double expected = 1.0;
  for (int i = 0; i < kSteps; ++i) {
    y = y * kC + x;
    expected = (expected * kC) + 1.0;
  }
  EXPECT_GT(tape.Memory().operands, 3 * kChunkEntries);
  tape.AddAdjoint(y, 1.0);
  tape.SweepBack(tape.GetPosition(), Position{});
  EXPECT_LE(Relative(tape.Adjoint(x), expected), 1e-12);
  tape.SetTangent(x, 1.0);
  tape.SweepForward(start);
  EXPECT_LE(Relative(tape.Tangent(y), expected), 1e-12);
}

// Recording at every chunk-boundary position, through both of Tape::Push's paths: the inline one
// and the one that enters a chunk. The probe is nine statements, two operands and one in turn
// (two is the most the tape takes):
//   z = -sqrt(exp(x0 x1) + x0) x1 / x0 + log(x1).
// It is recorded with each of its statements in turn as the first of a statement chunk, with its
// operands meeting the end of an operand chunk at every offset, with both at once, and far from
// both, and then again after a rewind, into chunks that already exist. After every statement the
// position must be where the layout puts it: a statement's operands go in the current operand
// chunk if they fit there, and start the next chunk otherwise. Values must be the double
// operations' bits; adjoints and tangents must be the bits of the same probe recorded on a fresh
// tape, and its analytic derivatives on libm within kProbeRelative.
constexpr std::array<OperandCount, 9> kProbeCounts = {2, 1, 2, 1, 2, 1, 2, 1, 2};
constexpr std::size_t kProbeOperands = 14;
constexpr double kX0 = 0.7;
constexpr double kX1 = 1.3;
// Nine statements whose partials are each within a few ULP (kAnalyticRelative's reasoning), and
// derivatives that lose at most a factor of 2 to cancellation: 1e-13 leaves room, while a lost
// operand or a partial on the wrong statement moves a derivative by order 1.
constexpr double kProbeRelative = 1e-13;

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

struct ProbeRun {
  Real z;
  std::array<double, 9> values{};
  std::array<Position, 9> positions{};
};

ProbeRun RecordProbe(const Tape& tape, const Real& x0, const Real& x1) {
  ProbeRun run;
  std::size_t i = 0;
  const auto at = [&](const Real& r) {
    run.values.at(i) = r.value();
    run.positions.at(i) = tape.GetPosition();
    ++i;
    return r;
  };
  const Real u = at(x0 * x1);
  const Real e = at(Exp(u));
  const Real s = at(e + x0);
  const Real r = at(Sqrt(s));
  const Real q = at(r * x1);
  const Real n = at(-q);
  const Real d = at(n / x0);
  const Real l = at(Log(x1));
  run.z = at(d + l);
  return run;
}

std::array<double, 9> ProbeValues() {
  const double u = kX0 * kX1;
  const double e = math::exp(u);
  const double s = e + kX0;
  const double r = math::sqrt(s);
  const double q = r * kX1;
  const double n = -q;
  const double d = n / kX0;
  const double l = math::log(kX1);
  return {u, e, s, r, q, n, d, l, d + l};
}

// dz/dx0 and dz/dx1 by hand, on libm.
std::array<double, 2> ProbeDerivatives() {
  const double e = std::exp(kX0 * kX1);
  const double r = std::sqrt(e + kX0);
  return {(-(kX1 / kX0) * ((kX1 * e) + 1.0) / (2.0 * r)) + (r * kX1 / (kX0 * kX0)),
          (-(r + (kX1 * kX0 * e / (2.0 * r))) / kX0) + (1.0 / kX1)};
}

struct ProbeGradient {
  std::array<double, 2> adjoints{};
  std::array<double, 2> tangents{};
};

// Sweeps the probe recorded since `from` both ways; leaves every adjoint and tangent at zero.
ProbeGradient SweepProbe(Tape& tape, const Position& from, const Real& z, const Real& x0,
                         const Real& x1) {
  ProbeGradient g;
  tape.AddAdjoint(z, 1.0);
  tape.SweepBack(tape.GetPosition(), from);
  g.adjoints = {tape.Adjoint(x0), tape.Adjoint(x1)};
  tape.ClearAdjoints(from, Position{});
  tape.SetTangent(x0, 1.0);
  tape.SweepForward(from);
  g.tangents[0] = tape.Tangent(z);
  tape.SetTangent(x0, 0.0);
  tape.SetTangent(x1, 1.0);
  tape.SweepForward(from);
  g.tangents[1] = tape.Tangent(z);
  tape.ClearTangents();
  return g;
}

std::string Describe(const Position& p) {
  return "statements " + std::to_string(p.statements) + ", operand chunk " +
         std::to_string(p.operand_chunk) + " offset " + std::to_string(p.operand_offset);
}

// Where the layout puts a statement with `count` operands recorded at `p`.
Position Advance(Position p, std::size_t count) {
  if (count > 0 && p.operand_offset + count > kChunkEntries) {
    p.operand_chunk += 1;
    p.operand_offset = 0;
  }
  p.operand_offset += count;
  p.statements += 1;
  return p;
}

// Fillers until the next statement is number `start` and `offset` operands fill chunk 0. They hang
// off their own variable, so they never reach the probe's derivatives: f f takes two operands, f 2
// one, a new variable none.
void Pad(Tape& tape, std::size_t start, std::size_t offset) {
  const Real f = tape.Register(0.5);
  const std::size_t fillers = start - tape.GetPosition().statements;
  const std::size_t two = offset > fillers ? offset - fillers : 0;
  const std::size_t one = offset - (2 * two);
  ASSERT_LE(one + two, fillers);
  for (std::size_t i = 0; i < two; ++i) static_cast<void>(f * f);
  for (std::size_t i = 0; i < one; ++i) static_cast<void>(f * 2.0);
  for (std::size_t i = one + two; i < fillers; ++i) static_cast<void>(tape.Register(1.0));
}

// Records the probe at `from` and checks each statement's position and value, then the gradient.
void CheckProbe(Tape& tape, const Position& from, const Real& x0, const Real& x1,
                const ProbeGradient& reference) {
  const std::array<double, 9> values = ProbeValues();
  const ProbeRun run = RecordProbe(tape, x0, x1);
  Position expected = from;
  for (std::size_t i = 0; i < kProbeCounts.size(); ++i) {
    expected = Advance(expected, kProbeCounts.at(i));
    ASSERT_EQ(Describe(run.positions.at(i)), Describe(expected)) << "statement " << i;
    EXPECT_EQ(Bits(run.values.at(i)), Bits(values.at(i))) << "statement " << i;
  }
  const ProbeGradient g = SweepProbe(tape, from, run.z, x0, x1);
  for (std::size_t k = 0; k < 2; ++k) {
    EXPECT_EQ(Bits(g.adjoints.at(k)), Bits(reference.adjoints.at(k))) << k;
    EXPECT_EQ(Bits(g.tangents.at(k)), Bits(reference.tangents.at(k))) << k;
  }
}

ProbeGradient FreshProbeGradient() {
  Tape tape;
  const TapeScope scope(tape);
  const Real x0 = tape.Register(kX0);
  const Real x1 = tape.Register(kX1);
  const Position from = tape.GetPosition();
  const ProbeRun run = RecordProbe(tape, x0, x1);
  return SweepProbe(tape, from, run.z, x0, x1);
}

TEST(Tape, RecordsAtEveryChunkBoundaryPosition) {
  const std::array<double, 2> derivatives = ProbeDerivatives();
  const ProbeGradient reference = FreshProbeGradient();
  for (std::size_t k = 0; k < 2; ++k) {
    EXPECT_LE(Relative(reference.adjoints.at(k), derivatives.at(k)), kProbeRelative) << k;
    EXPECT_LE(Relative(reference.tangents.at(k), derivatives.at(k)), kProbeRelative) << k;
  }

  // The probe's first statement number: each of its statements in turn the first of chunk 1, or
  // all of them inside chunk 1. Its first operand offset in chunk 0: every offset at which its 14
  // operands reach the end of the chunk (14 slots left down to 0, so the probe meets the boundary
  // before any filler has to fill a chunk exactly), or the middle of the chunk.
  std::vector<std::size_t> starts;
  starts.reserve(kProbeCounts.size() + 1);
  for (std::size_t j = 0; j < kProbeCounts.size(); ++j) starts.push_back(kChunkEntries - j);
  starts.push_back(kChunkEntries + (kChunkEntries / 2));
  std::vector<std::size_t> offsets;
  offsets.reserve(kProbeOperands + 2);
  for (std::size_t left = kProbeOperands + 1; left-- > 0;) offsets.push_back(kChunkEntries - left);
  offsets.push_back(kChunkEntries / 2);

  for (const std::size_t start : starts) {
    for (const std::size_t offset : offsets) {
      SCOPED_TRACE("first statement " + std::to_string(start) + ", first operand offset " +
                   std::to_string(offset));
      Tape tape;
      const TapeScope scope(tape);
      const Real x0 = tape.Register(kX0);
      const Real x1 = tape.Register(kX1);
      Pad(tape, start, offset);
      const Position from = tape.GetPosition();
      ASSERT_EQ(Describe(from), Describe({.statements = start, .operand_offset = offset}));
      for (const char* pass : {"first recording", "after a rewind"}) {
        SCOPED_TRACE(pass);
        CheckProbe(tape, from, x0, x1, reference);
        if (HasFatalFailure()) return;
        tape.Rewind(from);
      }
    }
  }
}

// The reverse sweep's paths: one and two operands, each straight-line, and the loop for wider
// statements, here three and kMaxOperands, in both SweepBack and SweepBackSkippingZeros. The
// reference is the sweep's definition on plain doubles (ReferenceSweep), and the adjoints must be
// its bits. The program has operands used twice in one statement with partials whose
// contributions round differently in the other order, statements whose adjoint is not zero and
// ones nothing uses, whose adjoint is zero; three of those have an infinite partial on an input
// of their own, so the plain sweep makes that input's adjoint NaN and the skip leaves it at zero.
// Two new variables sit among the statements, one used and one last, since the plain sweep carries
// the next statement's adjoint and must reload it past a statement with no operands. It is recorded
// at chunk boundaries as RecordsAtEveryChunkBoundaryPosition is, and again after a rewind.
struct ProgramStatement {
  std::vector<std::size_t> operands;  // Slots: the inputs, then the statements in order.
  std::vector<double> partials;
};

constexpr std::size_t kProgramInputs = 5;  // The last is used only by the unused statements.

std::vector<ProgramStatement> SweepProgram() {
  const auto s = [](std::size_t statement) { return kProgramInputs + statement; };
  const double inf = std::numeric_limits<double>::infinity();
  const auto wide = [](std::vector<std::size_t> cycle, std::size_t odd, double odd_partial) {
    ProgramStatement w;
    for (std::size_t j = 0; j < kMaxOperands; ++j) {
      w.operands.push_back(cycle[j % cycle.size()]);
      const double sign = j % 2 == 0 ? 1.0 : -1.0;
      w.partials.push_back(j == odd ? odd_partial : sign * static_cast<double>((j % 7) + 1) / 9.0);
    }
    return w;
  };
  return {
      {.operands = {0, 1}, .partials = {0.3, -1.7}},                     // 0
      {.operands = {s(0)}, .partials = {1.0 / 3.0}},                     // 1
      {.operands = {s(1), s(1)}, .partials = {0.1, -0.7}},               // 2
      {.operands = {s(2), 2, s(2)}, .partials = {1.3, 0.25, -0.65}},     // 3
      {},                                                                // 4, a new variable
      {.operands = {4}, .partials = {inf}},                              // 5, unused
      wide({s(3), 0, s(1), s(2), 1, s(4)}, 0, -0.6),                     // 6
      {.operands = {s(6), 3}, .partials = {0.5, -0.25}},                 // 7
      {.operands = {s(7)}, .partials = {-0.75}},                         // 8, the output
      {.operands = {s(6), 4}, .partials = {1.5, inf}},                   // 9, unused
      {.operands = {s(3), 2, s(3)}, .partials = {0.4, 0.2, -1.3}},       // 10, unused
      wide({s(3), 4, s(0)}, 4, inf),                                     // 11, unused
      {.operands = {s(2), s(7), s(2)}, .partials = {-0.6, 0.35, 0.15}},  // 12, unused
      {},                                                                // 13, a new variable
  };
}

// Statement 8 is the output; statement 3 is seeded too, so an adjoint mid-program has two parts.
constexpr std::array<std::pair<std::size_t, double>, 2> kProgramSeeds = {
    std::pair{kProgramInputs + 8, 1.0}, std::pair{kProgramInputs + 3, -0.5}};

// The sweep by its definition: statements last first; a new variable keeps its adjoint; any other
// statement's adjoint is read and zeroed, and then, unless it is zero and `skip`, each operand's
// adjoint gets the adjoint times its partial added, first operand first. With `reversed` set,
// statements of that many operands add theirs last operand first instead, which the test uses to
// show that the program tells the orders apart.
std::vector<double> ReferenceSweep(const std::vector<ProgramStatement>& program, bool skip,
                                   std::size_t reversed = 0) {
  std::vector<double> adjoints(kProgramInputs + program.size(), 0.0);
  for (const auto& [slot, seed] : kProgramSeeds) adjoints.at(slot) += seed;
  for (std::size_t k = program.size(); k-- > 0;) {
    const ProgramStatement& statement = program.at(k);
    const std::size_t n = statement.operands.size();
    if (n == 0) continue;
    const double adjoint = adjoints.at(kProgramInputs + k);
    adjoints.at(kProgramInputs + k) = 0.0;
    if (skip && adjoint == 0.0) continue;
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t j = n == reversed ? n - 1 - i : i;
      adjoints.at(statement.operands.at(j)) += adjoint * statement.partials.at(j);
    }
  }
  return adjoints;
}

// A new variable is registered; one and two operands go through the pushes real.hpp uses, and
// wider statements through the general one.
Real PushStatement(Tape& tape, const std::vector<Real>& slots, const ProgramStatement& statement,
                   double value) {
  const std::vector<std::size_t>& o = statement.operands;
  const std::vector<double>& p = statement.partials;
  if (o.empty()) return tape.Register(value);
  if (o.size() == 1) return tape.Push(value, slots.at(o[0]).index(), p[0]);
  if (o.size() == 2) {
    return tape.Push(value, slots.at(o[0]).index(), p[0], slots.at(o[1]).index(), p[1]);
  }
  std::vector<Index> indices;
  indices.reserve(o.size());
  for (const std::size_t slot : o) indices.push_back(slots.at(slot).index());
  return tape.Push(value, indices, p);
}

// Bits, except that a NaN matches any NaN: which NaN 0 times infinity gives is the hardware's.
bool SameBits(double a, double b) { return Bits(a) == Bits(b) || (std::isnan(a) && std::isnan(b)); }

// Records the program at `from`, checking each statement's position, then sweeps it (skipping
// zeros if `skip`) and checks every slot's adjoint against `reference`. It leaves every adjoint
// at zero, the new variables' among the statements too, so that a rewind to `from` may follow.
void CheckProgramSweep(Tape& tape, const Position& from, const std::vector<Real>& inputs,
                       const std::vector<ProgramStatement>& program, bool skip,
                       const std::vector<double>& reference) {
  std::vector<Real> slots = inputs;
  Position expected = from;
  for (std::size_t k = 0; k < program.size(); ++k) {
    slots.push_back(PushStatement(tape, slots, program.at(k), 1.0 + static_cast<double>(k)));
    expected = Advance(expected, program.at(k).operands.size());
    ASSERT_EQ(Describe(tape.GetPosition()), Describe(expected)) << "statement " << k;
  }
  for (const auto& [slot, seed] : kProgramSeeds) tape.AddAdjoint(slots.at(slot), seed);
  if (skip) {
    tape.SweepBackSkippingZeros(tape.GetPosition(), from);
  } else {
    tape.SweepBack(tape.GetPosition(), from);
  }
  for (std::size_t slot = 0; slot < slots.size(); ++slot) {
    const double adjoint = tape.Adjoint(slots.at(slot));
    EXPECT_TRUE(SameBits(adjoint, reference.at(slot)))
        << "slot " << slot << ": " << adjoint << " against " << reference.at(slot);
  }
  tape.ClearAdjoints(tape.GetPosition(), Position{});
}

// The program exercises what it says: an input NaN under the plain sweep only, an adjoint that is
// not zero on every other input and on the new variable mid-program, and, for two, three and
// kMaxOperands operands, a slot whose bits change when those statements add their contributions
// in the other order.
void CheckTheProgramTellsItsCasesApart(const std::vector<ProgramStatement>& program,
                                       const std::array<std::vector<double>, 2>& references) {
  constexpr std::size_t kNaNInput = kProgramInputs - 1;
  ASSERT_TRUE(std::isnan(references[0].at(kNaNInput)));
  ASSERT_EQ(Bits(references[1].at(kNaNInput)), Bits(0.0));
  for (std::size_t i = 0; i < kNaNInput; ++i) ASSERT_NE(references[0].at(i), 0.0) << i;
  ASSERT_NE(references[0].at(kProgramInputs + 4), 0.0);
  for (const bool skip : {false, true}) {
    const std::vector<double>& reference = references.at(skip ? 1 : 0);
    for (const std::size_t count : {std::size_t{2}, std::size_t{3}, kMaxOperands}) {
      const std::vector<double> reversed = ReferenceSweep(program, skip, count);
      bool differs = false;
      for (std::size_t i = 0; i < reference.size(); ++i) {
        differs = differs || !SameBits(reversed.at(i), reference.at(i));
      }
      ASSERT_TRUE(differs) << count << " operands, skip " << skip;
    }
  }
}

// First statements: each of the program's entries in turn the first of statement chunk 1, or all
// inside it.
std::vector<std::size_t> ProgramStarts(const std::vector<ProgramStatement>& program) {
  std::vector<std::size_t> starts;
  starts.reserve(program.size() + 1);
  for (std::size_t k = 0; k < program.size(); ++k) starts.push_back(kChunkEntries - k);
  starts.push_back(kChunkEntries + (kChunkEntries / 2));
  return starts;
}

// First operand offsets: for each statement, the one at which its operands just fit in operand
// chunk 0 and the one at which they just do not, an empty chunk 0, and the middle of the chunk.
std::vector<std::size_t> ProgramOffsets(const std::vector<ProgramStatement>& program) {
  std::vector<std::size_t> lefts = {0};
  lefts.reserve(1 + (2 * program.size()));
  std::size_t before = 0;
  for (const ProgramStatement& statement : program) {
    if (statement.operands.empty()) continue;
    before += statement.operands.size();
    lefts.push_back(before - 1);
    lefts.push_back(before);
  }
  std::vector<std::size_t> offsets;
  offsets.reserve(lefts.size() + 1);
  for (const std::size_t left : lefts) offsets.push_back(kChunkEntries - left);
  offsets.push_back(kChunkEntries / 2);
  return offsets;
}

// On a new tape: the inputs, fillers up to `start` and `offset`, then the program, swept and
// checked twice, the second time after a rewind.
void CheckProgramAt(std::size_t start, std::size_t offset,
                    const std::vector<ProgramStatement>& program, bool skip,
                    const std::vector<double>& reference) {
  constexpr std::array<double, kProgramInputs> kValues = {0.7, 1.3, -0.4, 2.1, 0.9};
  Tape tape;
  const TapeScope scope(tape);
  std::vector<Real> inputs;
  inputs.reserve(kProgramInputs);
  for (const double v : kValues) inputs.push_back(tape.Register(v));
  Pad(tape, start, offset);
  const Position from = tape.GetPosition();
  ASSERT_EQ(Describe(from), Describe({.statements = start, .operand_offset = offset}));
  for (const char* pass : {"first recording", "after a rewind"}) {
    SCOPED_TRACE(pass);
    CheckProgramSweep(tape, from, inputs, program, skip, reference);
    if (::testing::Test::HasFatalFailure()) return;
    tape.Rewind(from);
  }
}

TEST(Tape, SweepsStatementsOfEveryOperandCountInOrder) {
  const std::vector<ProgramStatement> program = SweepProgram();
  const std::array<std::vector<double>, 2> references = {ReferenceSweep(program, false),
                                                         ReferenceSweep(program, true)};
  CheckTheProgramTellsItsCasesApart(program, references);
  if (HasFatalFailure()) return;
  const std::vector<std::size_t> starts = ProgramStarts(program);
  const std::vector<std::size_t> offsets = ProgramOffsets(program);
  for (const bool skip : {false, true}) {
    for (const std::size_t start : starts) {
      for (const std::size_t offset : offsets) {
        SCOPED_TRACE(std::string(skip ? "skipping zeros" : "plain") + ", first statement " +
                     std::to_string(start) + ", first operand offset " + std::to_string(offset));
        CheckProgramAt(start, offset, program, skip, references.at(skip ? 1 : 0));
        if (HasFatalFailure()) return;
      }
    }
  }
}

TEST(TapeDeathTest, AStatementOfNoOrTooManyOperandsIsAnError) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  const std::vector<Index> wide(kMaxOperands + 1, 1);
  const std::vector<double> partials(kMaxOperands + 1, 1.0);
  EXPECT_DEATH(
      {
        Tape tape;
        static_cast<void>(tape.Register(1.0));
        static_cast<void>(tape.Push(0.0, std::span(wide).first(0), std::span(partials).first(0)));
      },
      "a statement takes 1 to 255 operands");
  EXPECT_DEATH(
      {
        Tape tape;
        static_cast<void>(tape.Register(1.0));
        static_cast<void>(tape.Push(0.0, wide, partials));
      },
      "a statement takes 1 to 255 operands");
}

TEST(Tape, RewindKeepsMemoryAndReRecordsTheSame) {
  Tape tape;
  const TapeScope scope(tape);
  const Real x = tape.Register(1.25);
  const Position mark = tape.GetPosition();
  std::size_t allocated = 0;
  for (int path = 0; path < 50; ++path) {
    Real y = x;
    for (std::size_t i = 0; i < kChunkEntries + 100; ++i) y = Exp(y * 0.001) + x;
    tape.AddAdjoint(y, 1.0);
    tape.SweepBack(tape.GetPosition(), mark);
    if (path == 0) allocated = tape.Memory().allocated_bytes;
    tape.Rewind(mark);
    EXPECT_EQ(tape.GetPosition(), mark);
    EXPECT_EQ(tape.Memory().allocated_bytes, allocated);
  }
}

// The set-up is swept once, from the mark to the start, after every path has added into its
// adjoints, and that equals the sum of the paths' gradients taken one at a time.
TEST(Tape, OneSetUpSweepEqualsTheSumOfPerPathSweeps) {
  constexpr int kPaths = 20;
  const auto path_value = [](const Real& s, const Real& v, int path) {
    return Exp(s * (0.01 * path)) * v + Log(s + v);
  };
  double accumulated_a = 0.0;
  double accumulated_b = 0.0;
  {
    Tape tape;
    const TapeScope scope(tape);
    const Real a = tape.Register(1.1);
    const Real b = tape.Register(0.4);
    const Real s = a * b;  // set-up
    const Real v = Sqrt(a + b);
    const Position mark = tape.GetPosition();
    for (int path = 0; path < kPaths; ++path) {
      tape.AddAdjoint(path_value(s, v, path), 1.0);
      tape.SweepBack(tape.GetPosition(), mark);
      tape.Rewind(mark);
    }
    tape.SweepBack(mark, Position{});
    accumulated_a = tape.Adjoint(a);
    accumulated_b = tape.Adjoint(b);
  }
  double sum_a = 0.0;
  double sum_b = 0.0;
  for (int path = 0; path < kPaths; ++path) {
    Tape tape;
    const TapeScope scope(tape);
    const Real a = tape.Register(1.1);
    const Real b = tape.Register(0.4);
    tape.AddAdjoint(path_value(a * b, Sqrt(a + b), path), 1.0);
    tape.SweepBack(tape.GetPosition(), Position{});
    sum_a += tape.Adjoint(a);
    sum_b += tape.Adjoint(b);
  }
  EXPECT_LE(Relative(accumulated_a, sum_a), 1e-13);
  EXPECT_LE(Relative(accumulated_b, sum_b), 1e-13);
}

TEST(Tape, MemoryReportCountsStatementsAndOperands) {
  Tape tape;
  const TapeScope scope(tape);
  const Real a = tape.Register(1.0);
  const Real b = tape.Register(2.0);
  static_cast<void>(a * b + Exp(a));  // three statements, five operands
  const MemoryReport m = tape.Memory();
  EXPECT_EQ(m.statements, 5U);
  EXPECT_EQ(m.operands, 5U);
  EXPECT_EQ(m.statement_bytes, 5 * kStatementBytes);
  EXPECT_EQ(m.operand_bytes, 5 * kOperandBytes);
  EXPECT_EQ(m.adjoint_bytes, 5 * sizeof(double));
  EXPECT_GE(m.allocated_bytes, kChunkEntries * (kStatementBytes + kOperandBytes + 8));
}

TEST(TapeDeathTest, ASecondActivationOnOneThreadIsAnError) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        Tape first;
        Tape second;
        const TapeScope one(first);
        const TapeScope two(second);
      },
      "a second tape activated on one thread");
}

TEST(TapeDeathTest, AnActiveRealWithNoActiveTapeIsAnError) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        Real x;
        Tape tape;
        {
          const TapeScope scope(tape);
          x = tape.Register(1.0);
        }
        static_cast<void>(Exp(x));
      },
      "no active tape");
}

}  // namespace
}  // namespace aadp::ad
