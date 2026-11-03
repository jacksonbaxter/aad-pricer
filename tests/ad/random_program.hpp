// Random straight-line programs over the tape's operations, for the dot-product test and the
// operation-sequence fuzzer (EVIDENCE B20, B23).
//
// Bytes, all read modulo what they choose from, so every byte string is a program:
//   [0]          inputs n = 1 + b % 4        [1]    outputs m = 1 + b % 3
//   2 bytes each value of input i: int16 / 4096, in [-8, 8)
//   1 byte each  tangent direction of input i: int8 / 64
//   1 byte each  weight of output j: int8 / 32 (zero allowed)
//   5 bytes each step: op, then operand selectors a, b, c, d (and d is the constant, int8 / 16)
// The outputs are the last m values. At most kMaxSteps steps.
//
// Safe domains. Decoding evaluates the program in double at the given inputs as it goes. An
// operation with a restricted domain applies to its operand directly only if the operand is
// inside the domain by a margin; otherwise the decoder first maps it in with steps of its own
// (log(a a + 1), sqrt(a a + 1), c / (b b + 1), InverseNormal(NormalCdf(s a)) with |s a| <= 4, and
// exp and the normal CDF of a scaled operand). Decoding stops before a step whose value would be
// non-finite or above kMaxMagnitude, so every decoded program is finite at its own inputs.
//
// Error model (ErrorModel below), independent of the tape: forward first-order propagation of
// the rounding of every step, |d op| times its operands' bounds plus c_op u |value|, with c_op
// from the accuracy of each function (src/math/ and normal_cdf.hpp), and of the absolute
// derivative sum_paths |product of partials| per input.
#ifndef AADP_TESTS_AD_RANDOM_PROGRAM_HPP_
#define AADP_TESTS_AD_RANDOM_PROGRAM_HPP_

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "math/exp.hpp"
#include "pricer/real_ops.hpp"

namespace aadp::ad::testing {

inline constexpr std::size_t kMaxInputs = 4;
inline constexpr std::size_t kMaxSteps = 64;
inline constexpr double kMaxMagnitude = 1e6;

enum class Op : std::uint8_t {
  kAdd,
  kSub,
  kMul,
  kDiv,
  kNeg,
  kExp,
  kLog,
  kSqrt,
  kNormalCdf,
  kInverseNormal,
  kMin,
  kMax,
  kSelect,  // values[c] < values[d] ? values[a] : values[b]
  kAddConstant,
  kMulConstant,
  kConstantMinus,
  kConstantOver,
  kSquare,
  kCount,
};

struct Step {
  Op op = Op::kAdd;
  std::uint16_t a = 0;
  std::uint16_t b = 0;
  std::uint16_t c = 0;
  std::uint16_t d = 0;
  double constant = 0.0;
};

struct Program {
  std::vector<double> inputs;
  std::vector<double> direction;  // A tangent direction, one per input.
  std::vector<Step> steps;
  std::vector<std::pair<std::size_t, double>> outputs;  // Value index and weight.
};

// Margins of the domain operations at the decoding point. Away from it, half the margin.
inline constexpr double kLogMargin = 1e-2;          // log, sqrt: argument at least this
inline constexpr double kDivisorMargin = 1e-2;      // |divisor| at least this
inline constexpr double kProbabilityMargin = 1e-6;  // inverse normal: inside [m, 1 - m]
inline constexpr double kExpLimit = 20.0;           // |exp argument| at most this
inline constexpr double kCdfLimit = 8.0;            // |normal CDF argument| at most this

// Applies one step on values[0..n) and returns its value. Masks of comparisons go to mask.
template <class T>
T Apply(const Step& s, const std::vector<T>& v, bool& mask) {
  using pricer::Exp;
  using pricer::InverseNormal;
  using pricer::Log;
  using pricer::Max;
  using pricer::Min;
  using pricer::NormalCdf;
  using pricer::Select;
  using pricer::Sqrt;
  const T& a = v[s.a];
  const T& b = v[s.b];
  mask = false;
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
      mask = b < a;
      return Min(a, b);
    case Op::kMax:
      mask = a < b;
      return Max(a, b);
    case Op::kSelect:
      mask = v[s.c] < v[s.d];
      return Select(mask, a, b);
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

// Whether a step's operand is inside its domain, by margin times scale, at these values.
inline bool InDomain(const Step& s, const std::vector<double>& v, double scale) {
  const double a = v[s.a];
  switch (s.op) {
    case Op::kLog:
    case Op::kSqrt:
      return a >= kLogMargin * scale;
    case Op::kDiv:
      return std::fabs(v[s.b]) >= kDivisorMargin * scale;
    case Op::kConstantOver:
      return std::fabs(a) >= kDivisorMargin * scale;
    case Op::kInverseNormal:
      return a >= kProbabilityMargin * scale && a <= 1.0 - (kProbabilityMargin * scale);
    case Op::kExp:
      return std::fabs(a) <= kExpLimit / scale;
    case Op::kNormalCdf:
      return std::fabs(a) <= kCdfLimit / scale;
    default:
      return true;
  }
}

namespace program_detail {

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
  std::uint8_t Byte() { return position_ < bytes_.size() ? bytes_[position_++] : 0; }
  std::int8_t Signed() { return static_cast<std::int8_t>(Byte()); }
  std::int16_t Signed16() {
    const auto lo = static_cast<std::uint16_t>(Byte());
    const auto hi = static_cast<std::uint16_t>(Byte());
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | (hi << 8U)));
  }
  [[nodiscard]] bool Done() const { return position_ >= bytes_.size(); }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t position_ = 0;
};

inline std::uint16_t Pick(std::uint8_t byte, std::size_t count) {
  return static_cast<std::uint16_t>(byte % count);
}

}  // namespace program_detail

// Appends s if its value is finite and within kMaxMagnitude. Returns whether it did.
inline bool Append(Program& p, std::vector<double>& values, const Step& s) {
  bool mask = false;
  const double value = Apply(s, values, mask);
  if (!std::isfinite(value) || std::fabs(value) > kMaxMagnitude) return false;
  if (p.steps.size() >= kMaxSteps) return false;
  p.steps.push_back(s);
  values.push_back(value);
  return true;
}

inline std::uint16_t Last(const std::vector<double>& values) {
  return static_cast<std::uint16_t>(values.size() - 1);
}

// Maps operand a into the domain of op with steps of its own, then appends op on the result.
inline bool AppendGuarded(Program& p, std::vector<double>& values, Step s) {
  if (InDomain(s, values, 1.0)) return Append(p, values, s);
  const auto unary = [&](Op op, std::uint16_t a, double constant = 0.0) {
    return Append(p, values, Step{.op = op, .a = a, .b = a, .c = a, .d = a, .constant = constant});
  };
  switch (s.op) {
    case Op::kLog:
    case Op::kSqrt:
    case Op::kConstantOver:
      if (!unary(Op::kSquare, s.a) || !unary(Op::kAddConstant, Last(values), 1.0)) return false;
      s.a = Last(values);
      return Append(p, values, s);
    case Op::kDiv:
      if (!unary(Op::kSquare, s.b) || !unary(Op::kAddConstant, Last(values), 1.0)) return false;
      s.b = Last(values);
      return Append(p, values, s);
    case Op::kExp:
    case Op::kNormalCdf: {
      const double limit = s.op == Op::kExp ? kExpLimit : kCdfLimit;
      if (!unary(Op::kMulConstant, s.a, limit / std::fabs(values[s.a]))) return false;
      s.a = Last(values);
      return Append(p, values, s);
    }
    case Op::kInverseNormal: {
      const double magnitude = std::fabs(values[s.a]);
      if (magnitude > 4.0) {
        if (!unary(Op::kMulConstant, s.a, 4.0 / magnitude)) return false;
        s.a = Last(values);
      }
      if (!unary(Op::kNormalCdf, s.a)) return false;
      s.a = Last(values);
      return Append(p, values, s);
    }
    default:
      return Append(p, values, s);
  }
}

inline Program Decode(std::span<const std::uint8_t> bytes) {
  program_detail::Reader in(bytes);
  Program p;
  const std::size_t n = 1 + (in.Byte() % kMaxInputs);
  const std::size_t m = 1 + (in.Byte() % 3);
  for (std::size_t i = 0; i < n; ++i) p.inputs.push_back(in.Signed16() / 4096.0);
  for (std::size_t i = 0; i < n; ++i) p.direction.push_back(in.Signed() / 64.0);
  std::vector<double> weights;
  for (std::size_t j = 0; j < m; ++j) weights.push_back(in.Signed() / 32.0);
  std::vector<double> values = p.inputs;
  while (!in.Done() && p.steps.size() < kMaxSteps) {
    const auto op = static_cast<Op>(in.Byte() % static_cast<std::uint8_t>(Op::kCount));
    const std::size_t count = values.size();
    Step s{.op = op,
           .a = program_detail::Pick(in.Byte(), count),
           .b = program_detail::Pick(in.Byte(), count),
           .c = program_detail::Pick(in.Byte(), count),
           .d = 0,
           .constant = 0.0};
    const std::uint8_t last = in.Byte();
    s.d = program_detail::Pick(last, count);
    s.constant = static_cast<std::int8_t>(last) / 16.0;
    if (!AppendGuarded(p, values, s)) break;
  }
  for (std::size_t j = 0; j < m && j < values.size(); ++j) {
    p.outputs.emplace_back(values.size() - 1 - j, weights[j]);
  }
  return p;
}

// Values of every input and step, and the comparison masks, at the given inputs.
template <class T>
struct Trace {
  std::vector<T> values;
  std::vector<bool> masks;
  bool in_domain = true;  // At half the decoding margins.
};

template <class T>
void Run(const Program& p, std::span<const T> inputs, Trace<T>& out) {
  out.values.assign(inputs.begin(), inputs.end());
  out.masks.clear();
  out.in_domain = true;
  for (const Step& s : p.steps) {
    if constexpr (std::is_same_v<T, double>) {
      out.in_domain = out.in_domain && InDomain(s, out.values, 0.5);
    }
    bool mask = false;
    const T value = Apply(s, out.values, mask);
    out.values.push_back(value);
    out.masks.push_back(mask);
  }
}

// First-order bounds at the decoding point (see the header comment).
struct ErrorModel {
  std::vector<double> error;                               // per value, absolute
  std::vector<std::array<double, kMaxInputs>> derivative;  // per value, |d value / d input|
};

inline double NormalPdf(double x) {
  return std::exp(-0.5 * x * x) / std::sqrt(2.0 * std::numbers::pi);
}

inline ErrorModel Model(const Program& p, const std::vector<double>& v) {
  constexpr double u = 0x1p-53;
  ErrorModel model;
  const std::size_t n = p.inputs.size();
  model.error.assign(n, 0.0);
  model.derivative.assign(n, {});
  for (std::size_t i = 0; i < n; ++i) model.derivative[i][i] = 1.0;
  for (std::size_t k = 0; k < p.steps.size(); ++k) {
    const Step& s = p.steps[k];
    const double a = v[s.a];
    const double b = v[s.b];
    const double y = v[n + k];
    // |partial| on a and on b, and the step's own rounding in units of u |y|.
    double da = 0.0;
    double db = 0.0;
    double rounding = 1.0;
    std::size_t chosen = s.a;
    bool select = false;
    switch (s.op) {
      case Op::kAdd:
      case Op::kSub:
        da = 1.0;
        db = 1.0;
        break;
      case Op::kMul:
        da = std::fabs(b);
        db = std::fabs(a);
        break;
      case Op::kDiv:
        da = 1.0 / std::fabs(b);
        db = std::fabs(a) / (b * b);
        break;
      case Op::kNeg:
        da = 1.0;
        rounding = 0.0;
        break;
      case Op::kExp:
        da = std::fabs(y);
        rounding = 4.0;  // 0.613 ULP (stage 1a), at most 2 u |y| per ULP
        break;
      case Op::kLog:
        da = 1.0 / std::fabs(a);
        rounding = 4.0;  // 0.840 ULP
        break;
      case Op::kSqrt:
        da = 0.5 / std::fabs(y);
        break;
      case Op::kNormalCdf:
        da = NormalPdf(a);
        // erfc within 4 ULP, and 4 y^2 u from rounding y = -x / sqrt 2 (normal_cdf.hpp).
        rounding = 16.0 + (2.0 * a * a);
        break;
      case Op::kInverseNormal:
        da = 1.0 / NormalPdf(y);
        rounding = 16.0;  // AS241 within 1e-15 relative
        break;
      case Op::kMin:
        chosen = b < a ? s.b : s.a;
        select = true;
        break;
      case Op::kMax:
        chosen = a < b ? s.b : s.a;
        select = true;
        break;
      case Op::kSelect:
        chosen = v[s.c] < v[s.d] ? s.a : s.b;
        select = true;
        break;
      case Op::kAddConstant:
      case Op::kConstantMinus:
        da = 1.0;
        break;
      case Op::kMulConstant:
        da = std::fabs(s.constant);
        break;
      case Op::kConstantOver:
        da = std::fabs(s.constant) / (a * a);
        break;
      case Op::kSquare:
        da = 2.0 * std::fabs(a);
        break;
      case Op::kCount:
        break;
    }
    std::array<double, kMaxInputs> derivative{};
    double error = 0.0;
    if (select) {
      derivative = model.derivative[chosen];
      error = model.error[chosen];
    } else {
      const bool binary =
          s.op == Op::kAdd || s.op == Op::kSub || s.op == Op::kMul || s.op == Op::kDiv;
      for (std::size_t i = 0; i < n; ++i) {
        derivative[i] = da * model.derivative[s.a][i];
        if (binary) derivative[i] += db * model.derivative[s.b][i];
      }
      error = (da * model.error[s.a]) + (binary ? db * model.error[s.b] : 0.0) +
              (rounding * u * std::fabs(y)) + 0x1p-1074;
    }
    model.derivative.push_back(derivative);
    model.error.push_back(error);
  }
  return model;
}

}  // namespace aadp::ad::testing

#endif  // AADP_TESTS_AD_RANDOM_PROGRAM_HPP_
