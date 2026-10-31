// Generic harness, bar item B19: one table of operations, each a generic lambda instantiated on
// double and on Dual. Every operation is checked against finite differences, and against double
// at NaN, inf and zero.
//
// Finite-difference tolerance is fixed from the method: the five-point stencil has truncation
// h^4 |f^(5)| / 30 and round-off about 1e-16 |f| / h. The closest input to a singularity is
// 0.01 away, where truncation at h = 1e-4 is about 3e-10 |f'|, and round-off is 1e-12 |f|. So
// 1e-8 relative to 1 + |derivative| is a margin a wrong chain rule still cannot meet.
#include "support/dual.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace aadp::test {
namespace {

using std::exp;
using std::expm1;
using std::fabs;
using std::log;
using std::log1p;
using std::max;
using std::min;
using std::pow;
using std::sqrt;

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

struct UnaryOp {
  std::string name;
  std::function<double(double)> on_double;
  std::function<Dual(const Dual&)> on_dual;
  std::vector<double> smooth_points;
};

struct BinaryOp {
  std::string name;
  std::function<double(double, double)> on_double;
  std::function<Dual(const Dual&, const Dual&)> on_dual;
  std::vector<std::pair<double, double>> smooth_points;
};

template <class F>
UnaryOp Unary(std::string name, F f, std::vector<double> points) {
  return {std::move(name), [f](double x) { return f(x); }, [f](const Dual& x) { return f(x); },
          std::move(points)};
}

template <class F>
BinaryOp Binary(std::string name, F f, std::vector<std::pair<double, double>> points) {
  return {std::move(name), [f](double a, double b) { return f(a, b); },
          [f](const Dual& a, const Dual& b) { return f(a, b); }, std::move(points)};
}

const std::vector<double> kEverywhere = {-3.0, -1.0, -0.25, 0.25, 0.5, 1.0, 2.5};
const std::vector<double> kPositive = {0.05, 0.25, 0.5, 1.0, 2.5, 40.0};
const std::vector<double> kProbability = {0.01, 0.1, 0.3, 0.5, 0.7, 0.99};

std::vector<UnaryOp> UnaryOps() {
  return {
      Unary(
          "negate", [](auto x) { return -x; }, kEverywhere),
      Unary(
          "plus", [](auto x) { return +x; }, kEverywhere),
      Unary(
          "exp", [](auto x) { return exp(x); }, kEverywhere),
      Unary(
          "expm1", [](auto x) { return expm1(x); }, kEverywhere),
      Unary(
          "log", [](auto x) { return log(x); }, kPositive),
      Unary("log1p", [](auto x) { return log1p(x); }, {-0.5, 0.0, 0.5, 3.0}),
      Unary(
          "sqrt", [](auto x) { return sqrt(x); }, kPositive),
      Unary(
          "fabs", [](auto x) { return fabs(x); }, kEverywhere),
      Unary(
          "normal_pdf", [](auto x) { return normal_pdf(x); }, kEverywhere),
      Unary(
          "normal_cdf", [](auto x) { return normal_cdf(x); }, kEverywhere),
      Unary(
          "inverse_normal", [](auto p) { return inverse_normal(p); }, kProbability),
      Unary(
          "pow_const_exponent", [](auto x) { return pow(x, 2.5); }, kPositive),
      Unary(
          "pow_const_base", [](auto x) { return pow(1.7, x); }, kEverywhere),
      Unary(
          "add_double", [](auto x) { return x + 2.0; }, kEverywhere),
      Unary(
          "double_add", [](auto x) { return 2.0 + x; }, kEverywhere),
      Unary(
          "sub_double", [](auto x) { return x - 2.0; }, kEverywhere),
      Unary(
          "double_sub", [](auto x) { return 2.0 - x; }, kEverywhere),
      Unary(
          "mul_double", [](auto x) { return x * 2.0; }, kEverywhere),
      Unary(
          "double_mul", [](auto x) { return 2.0 * x; }, kEverywhere),
      Unary(
          "div_double", [](auto x) { return x / 2.0; }, kEverywhere),
      Unary(
          "double_div", [](auto x) { return 2.0 / x; }, kEverywhere),
      Unary(
          "compound_add",
          [](auto x) {
            auto y = x;
            y += x * x;
            return y;
          },
          kEverywhere),
      Unary(
          "compound_sub",
          [](auto x) {
            auto y = x;
            y -= x * x;
            return y;
          },
          kEverywhere),
      Unary(
          "compound_mul",
          [](auto x) {
            auto y = x;
            y *= x;
            return y;
          },
          kEverywhere),
      Unary(
          "compound_div",
          [](auto x) {
            auto y = x + 3.5;
            y /= x;
            return y;
          },
          kEverywhere),
  };
}

std::vector<BinaryOp> BinaryOps() {
  const std::vector<std::pair<double, double>> points = {
      {0.5, 2.0}, {3.0, 0.25}, {-1.5, 2.5}, {2.0, -0.75}, {-0.5, -2.0}};
  const std::vector<std::pair<double, double>> distinct = {{0.5, 2.0}, {3.0, 0.25}, {-1.5, 2.5}};
  return {
      Binary(
          "add", [](auto a, auto b) { return a + b; }, points),
      Binary(
          "sub", [](auto a, auto b) { return a - b; }, points),
      Binary(
          "mul", [](auto a, auto b) { return a * b; }, points),
      Binary(
          "div", [](auto a, auto b) { return a / b; }, points),
      Binary("pow", [](auto a, auto b) { return pow(a, b); },
             {{0.5, 2.0}, {3.0, 0.25}, {2.0, -0.75}}),
      Binary(
          "max", [](auto a, auto b) { return max(a, b); }, distinct),
      Binary(
          "min", [](auto a, auto b) { return min(a, b); }, distinct),
  };
}

// Five-point stencil.
template <class F>
double Derivative(const F& f, double x) {
  constexpr double h = 1e-4;
  return (-f(x + 2 * h) + 8 * f(x + h) - 8 * f(x - h) + f(x - 2 * h)) / (12 * h);
}

constexpr double kFdTolerance = 1e-8;

TEST(DualGeneric, EveryUnaryOpMatchesFiniteDifferences) {
  for (const UnaryOp& op : UnaryOps()) {
    for (const double x : op.smooth_points) {
      for (std::size_t slot = 0; slot < kTangents; ++slot) {
        const Dual out = op.on_dual(Dual::Variable(x, slot));
        const double expected = Derivative(op.on_double, x);
        EXPECT_NEAR(out.tangent[slot], expected, kFdTolerance * (1.0 + std::fabs(expected)))
            << op.name << " at " << x << " slot " << slot;
        for (std::size_t other = 0; other < kTangents; ++other) {
          if (other != slot) EXPECT_EQ(out.tangent[other], 0.0) << op.name;
        }
      }
    }
  }
}

TEST(DualGeneric, EveryBinaryOpMatchesFiniteDifferences) {
  for (const BinaryOp& op : BinaryOps()) {
    for (const auto& [a, b] : op.smooth_points) {
      const Dual out = op.on_dual(Dual::Variable(a, 1), Dual::Variable(b, 3));
      const double along_a = Derivative([&](double x) { return op.on_double(x, b); }, a);
      const double along_b = Derivative([&](double x) { return op.on_double(a, x); }, b);
      EXPECT_NEAR(out.tangent[1], along_a, kFdTolerance * (1.0 + std::fabs(along_a))) << op.name;
      EXPECT_NEAR(out.tangent[3], along_b, kFdTolerance * (1.0 + std::fabs(along_b))) << op.name;
      EXPECT_EQ(out.tangent[0], 0.0) << op.name;
    }
  }
}

// Same bits, including the sign of zero and NaN-ness, so a Dual kernel takes every branch a
// double kernel takes.
bool SameValue(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
  return a == b && std::signbit(a) == std::signbit(b);
}

const std::vector<double> kSpecial = {0.0,  -0.0,   1.0,     -1.0,  kInf,  -kInf,
                                      kNan, 1e-320, -1e-320, 1e308, -1e308};

TEST(DualParity, ValuesMatchDoubleAtSpecialInputs) {
  for (const UnaryOp& op : UnaryOps()) {
    for (const double x : kSpecial) {
      EXPECT_TRUE(SameValue(op.on_dual(Dual(x)).value, op.on_double(x))) << op.name << " at " << x;
      EXPECT_TRUE(SameValue(op.on_dual(Dual::Variable(x, 2)).value, op.on_double(x)))
          << op.name << " at " << x;
    }
  }
  for (const BinaryOp& op : BinaryOps()) {
    for (const double a : kSpecial) {
      for (const double b : kSpecial) {
        EXPECT_TRUE(SameValue(op.on_dual(Dual(a), Dual(b)).value, op.on_double(a, b)))
            << op.name << " at " << a << ", " << b;
      }
    }
  }
}

// A constant has no derivative wherever its local derivative is inf or NaN.
TEST(DualParity, ZeroTangentStaysZero) {
  for (const UnaryOp& op : UnaryOps()) {
    for (const double x : kSpecial) {
      const Dual out = op.on_dual(Dual(x));
      for (const double t : out.tangent) EXPECT_EQ(t, 0.0) << op.name << " at " << x;
    }
  }
  for (const BinaryOp& op : BinaryOps()) {
    for (const double a : kSpecial) {
      for (const double b : kSpecial) {
        const Dual out = op.on_dual(Dual(a), Dual(b));
        for (const double t : out.tangent) EXPECT_EQ(t, 0.0) << op.name;
      }
    }
  }
}

// One-sided limits written out, so the IEEE behaviour on a live tangent is a stated contract.
TEST(DualParity, LiveTangentAtSingularities) {
  const auto seed = [](double x) { return Dual::Variable(x, 0); };
  EXPECT_EQ(sqrt(seed(0.0)).tangent[0], kInf);
  EXPECT_EQ(log(seed(0.0)).tangent[0], kInf);
  EXPECT_EQ(log(seed(0.0)).value, -kInf);
  EXPECT_TRUE(std::isnan(log(seed(-1.0)).value));
  EXPECT_EQ(inverse_normal(seed(0.0)).value, -kInf);
  EXPECT_TRUE(std::isnan(inverse_normal(seed(0.0)).tangent[0]) ||
              std::isinf(inverse_normal(seed(0.0)).tangent[0]));
  EXPECT_TRUE(std::isnan(inverse_normal(seed(kNan)).tangent[0]));
  EXPECT_TRUE(std::isnan(exp(seed(kNan)).tangent[0]));
  // A sum's local derivatives are 1 whatever the value, so the NaN stays in the value here. In a
  // product the NaN is itself a local derivative, so it reaches the tangent.
  EXPECT_TRUE(std::isnan((seed(kNan) + seed(1.0)).value));
  EXPECT_EQ((seed(kNan) + seed(1.0)).tangent[0], 2.0);
  EXPECT_TRUE(std::isnan((seed(kNan) * seed(1.0)).tangent[0]));
  EXPECT_EQ(exp(seed(-kInf)).tangent[0], 0.0);
  EXPECT_EQ(exp(seed(kInf)).tangent[0], kInf);
  EXPECT_EQ(normal_cdf(seed(kInf)).value, 1.0);
  EXPECT_EQ(normal_cdf(seed(kInf)).tangent[0], 0.0);
  EXPECT_EQ((seed(1.0) / Dual(0.0)).value, kInf);
  EXPECT_TRUE(std::isnan((seed(0.0) / seed(0.0)).value));
}

TEST(DualParity, ComparisonsMatchDouble) {
  for (const double a : kSpecial) {
    for (const double b : kSpecial) {
      const Dual x(a);
      const Dual y(b);
      EXPECT_EQ(x == y, a == b);
      EXPECT_EQ(x != y, a != b);
      EXPECT_EQ(x < y, a < b);
      EXPECT_EQ(x <= y, a <= b);
      EXPECT_EQ(x > y, a > b);
      EXPECT_EQ(x >= y, a >= b);
      EXPECT_EQ(x > b, a > b);
      EXPECT_EQ(a < y, a < b);
    }
  }
}

// Whole-expression check: products of the Phi route used by one-step survival.
TEST(DualChain, ComposedExpressionMatchesFiniteDifferences) {
  const auto f = [](auto x) {
    const auto p = normal_cdf(x);
    return log(p) * inverse_normal(p * 0.75) + exp(-x * x) / sqrt(1.0 + x * x);
  };
  for (const double x : {-1.5, -0.2, 0.3, 1.2}) {
    const double expected = Derivative([&](double v) { return f(v); }, x);
    EXPECT_NEAR(f(Dual::Variable(x, 4)).tangent[4], expected,
                kFdTolerance * (1.0 + std::fabs(expected)));
  }
}

TEST(DualSeeding, TangentsStayInTheirOwnSlots) {
  const Dual a = Dual::Variable(2.0, 0);
  const Dual b = Dual::Variable(3.0, 1);
  const Dual product = a * b;
  EXPECT_EQ(product.tangent[0], 3.0);
  EXPECT_EQ(product.tangent[1], 2.0);
  EXPECT_EQ(product.tangent[2], 0.0);
}

}  // namespace
}  // namespace aadp::test
