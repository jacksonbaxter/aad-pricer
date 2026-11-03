// Unit tests of the tape itself: every operation's partial in reverse and tangent mode
// (EVIDENCE B18), passive constants, Select, chunk boundaries, mark and rewind, the set-up sweep,
// and activation (B10).
#include "ad/tape.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <numbers>
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
