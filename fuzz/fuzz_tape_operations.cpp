// Operation sequences on the tape against central differences (EVIDENCE B23, the "exceeds" item).
//
// The input decodes to a random straight-line program over every operation the tape supports, on
// safe domains (tests/ad/random_program.hpp gives the byte layout). The tape records it at the
// decoded inputs, and its reverse sweep, seeded with the output weights, gives the gradient of
// f = sum_j w_j y_j. That is compared with central differences of the same program run in double.
//
// The bound, per input k, from step size and conditioning, stated before any run:
//   D_h = (f(x + h e_k) - f(x - h e_k)) / w_h, w_h the realised width, and D_2h the same at 2h,
//   with h = 2^-16 max(|x_k|, 1).
//   Truncation: D_h - f' = c h^2 + ..., so D_2h - D_h is about 3 c h^2, and R = |D_h - D_2h|
//   bounds the truncation at h three times over; it also carries the round-off of both.
//   Round-off: random_program.hpp's ErrorModel bounds the absolute error E of f at the decoding
//   point to first order, so each difference quotient carries at most about 2 E / w. Twice that
//   for the change of E across the stencil.
//   The tape's own association error, 2 gamma_n A (1 + 2 gamma_n) (bounds.hpp), A from the
//   ErrorModel's absolute derivative.
//   tol_k = 2 R + 4 E / w_h + 2 gamma_n A_k (1 + 2 gamma_n).
// Rejection rule, applied to the draw before its adjoint is compared, and never by retrying:
//   R1 a value at any stencil point is not finite or exceeds 1e6 in magnitude;
//   R2 a comparison (Min, Max, Select) differs between the decoding point and a stencil point,
//      so a kink lies inside the stencil;
//   R3 a domain operation's operand is within half its decoding margin at a stencil point;
//   R4 ill-conditioned: tol_k > 1e-6 A_k for some k with A_k > 0, so the differences cannot
//      resolve that derivative to a millionth of its scale.
// An input with A_k = 0 has every product of partials zero at this point (no path to the output,
// or a factor such as d(a a)/da at a = 0): its adjoint must be exactly zero, and D_h within tol_k.
// The value of every step on the tape must also be the double value, bit for bit, and the
// tangent-adjoint dot-product identity of check 2 must hold on the same tape.
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "ad/bounds.hpp"  // tests/ad/, through the test support include path
#include "ad/random_program.hpp"
#include "ad/real.hpp"
#include "ad/tape.hpp"

#define AADP_FUZZ_REQUIRE(condition, what)                                                   \
  do {                                                                                       \
    if (!(condition)) {                                                                      \
      std::fprintf(stderr, "invariant failed: %s: %s (%s:%d)\n", what, #condition, __FILE__, \
                   __LINE__);                                                                \
      std::abort();                                                                          \
    }                                                                                        \
  } while (false)

namespace {

using aadp::ad::Position;
using aadp::ad::Real;
using aadp::ad::Tape;
using aadp::ad::TapeScope;
namespace t = aadp::ad::testing;

constexpr double kConditioning = 1e-6;
constexpr double kStepScale = 0x1p-16;

enum Outcome : std::uint8_t { kAccepted, kR1, kR2, kR3, kR4, kOutcomes };
std::array<std::atomic<std::uint64_t>, kOutcomes> g_counts{};

void Count(Outcome outcome) {
  g_counts[outcome].fetch_add(1);
  std::uint64_t all = 0;
  for (const auto& c : g_counts) all += c.load();
  if ((all & ((std::uint64_t{1} << 17) - 1)) == 0) {
    std::fprintf(stderr,
                 "tape-operations: %llu draws: accepted %llu, rejected R1 %llu R2 %llu "
                 "R3 %llu R4 %llu\n",
                 static_cast<unsigned long long>(all),
                 static_cast<unsigned long long>(g_counts[kAccepted].load()),
                 static_cast<unsigned long long>(g_counts[kR1].load()),
                 static_cast<unsigned long long>(g_counts[kR2].load()),
                 static_cast<unsigned long long>(g_counts[kR3].load()),
                 static_cast<unsigned long long>(g_counts[kR4].load()));
  }
}

double Output(const t::Program& p, const std::vector<double>& values) {
  double f = 0.0;
  for (const auto& [index, weight] : p.outputs) f += weight * values[index];
  return f;
}

// f at x with input k moved by delta, or the reason the point is unusable.
Outcome Evaluate(const t::Program& p, const t::Trace<double>& base, std::size_t k, double delta,
                 double& x_k, double& f) {
  std::vector<double> x = p.inputs;
  x[k] += delta;
  x_k = x[k];
  t::Trace<double> trace;
  t::Run<double>(p, x, trace);
  for (const double v : trace.values) {
    if (!std::isfinite(v) || std::fabs(v) > t::kMaxMagnitude) return kR1;
  }
  if (trace.masks != base.masks) return kR2;
  if (!trace.in_domain) return kR3;
  f = Output(p, trace.values);
  return kAccepted;
}

struct Differences {
  double d_h = 0.0;
  double d_2h = 0.0;
  double width = 0.0;
};

Outcome Difference(const t::Program& p, const t::Trace<double>& base, std::size_t k,
                   Differences& out) {
  const double h = kStepScale * std::max(std::fabs(p.inputs[k]), 1.0);
  std::array<double, 4> f{};
  std::array<double, 4> x{};
  const std::array<double, 4> deltas = {h, -h, 2.0 * h, -2.0 * h};
  for (std::size_t i = 0; i < 4; ++i) {
    const Outcome o = Evaluate(p, base, k, deltas[i], x[i], f[i]);
    if (o != kAccepted) return o;
  }
  out.width = x[0] - x[1];
  out.d_h = (f[0] - f[1]) / out.width;
  out.d_2h = (f[2] - f[3]) / (x[2] - x[3]);
  return kAccepted;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const t::Program program = t::Decode(std::span(data, size));
  const std::size_t n = program.inputs.size();

  t::Trace<double> base;
  t::Run<double>(program, program.inputs, base);
  const t::ErrorModel model = t::Model(program, base.values);
  double error = 0.0;  // E, first order, including the weighted sum's own rounding
  double weighted = 0.0;
  std::array<double, t::kMaxInputs> absolute{};
  for (const auto& [index, weight] : program.outputs) {
    error += std::fabs(weight) * model.error[index];
    weighted += std::fabs(weight * base.values[index]);
    for (std::size_t k = 0; k < n; ++k) {
      absolute[k] += std::fabs(weight) * model.derivative[index][k];
    }
  }
  error += 4.0 * t::kUnitRoundoff * weighted;

  Tape tape;
  const TapeScope scope(tape);
  std::vector<Real> x;
  for (const double v : program.inputs) x.push_back(tape.Register(v));
  t::Trace<Real> trace;
  t::Run<Real>(program, x, trace);
  for (std::size_t i = 0; i < base.values.size(); ++i) {
    AADP_FUZZ_REQUIRE(std::bit_cast<std::uint64_t>(trace.values[i].value()) ==
                          std::bit_cast<std::uint64_t>(base.values[i]),
                      "tape value differs from the double program");
  }
  const Position end = tape.GetPosition();
  const aadp::ad::MemoryReport memory = tape.Memory();
  const double terms = t::RoundoffTerms(memory.statements, memory.operands);
  const double gamma = t::Gamma(terms + 8.0);

  for (const auto& [index, weight] : program.outputs) tape.AddAdjoint(trace.values[index], weight);
  tape.SweepBack(end, Position{});
  std::array<double, t::kMaxInputs> adjoint{};
  for (std::size_t k = 0; k < n; ++k) adjoint[k] = tape.Adjoint(x[k]);

  // The dot-product identity on the same tape (check 2), bound as there.
  for (std::size_t k = 0; k < n; ++k) tape.SetTangent(x[k], program.direction[k]);
  tape.SweepForward(Position{});
  double lhs = 0.0;
  for (const auto& [index, weight] : program.outputs) {
    lhs += weight * tape.Tangent(trace.values[index]);
  }
  double rhs = 0.0;
  double s = 0.0;
  for (std::size_t k = 0; k < n; ++k) {
    rhs += program.direction[k] * adjoint[k];
    s += std::fabs(program.direction[k]) * absolute[k];
  }
  AADP_FUZZ_REQUIRE(std::isfinite(lhs) && std::isfinite(rhs), "gradient not finite");
  const double dot_bound = (2.0 * gamma * s * (1.0 + (4.0 * gamma))) + ((terms + 8.0) * 0x1p-1070);
  AADP_FUZZ_REQUIRE(std::fabs(lhs - rhs) <= dot_bound, "dot-product identity");

  std::array<Differences, t::kMaxInputs> differences{};
  std::array<double, t::kMaxInputs> tolerance{};
  for (std::size_t k = 0; k < n; ++k) {
    const Outcome o = Difference(program, base, k, differences[k]);
    if (o != kAccepted) {
      Count(o);
      return 0;
    }
    const Differences& d = differences[k];
    tolerance[k] = (2.0 * std::fabs(d.d_h - d.d_2h)) + (4.0 * error / d.width) +
                   (2.0 * gamma * absolute[k] * (1.0 + (2.0 * gamma)));
    if (absolute[k] > 0.0 && tolerance[k] > kConditioning * absolute[k]) {
      Count(kR4);
      return 0;
    }
  }
  Count(kAccepted);
  for (std::size_t k = 0; k < n; ++k) {
    if (absolute[k] == 0.0) {
      AADP_FUZZ_REQUIRE(adjoint[k] == 0.0, "adjoint with every path product zero is not zero");
      AADP_FUZZ_REQUIRE(std::fabs(differences[k].d_h) <= tolerance[k],
                        "difference with every path product zero is not within tolerance");
      continue;
    }
    if (!(std::fabs(adjoint[k] - differences[k].d_h) <= tolerance[k])) {
      std::fprintf(stderr, "input %zu: adjoint %.17g, D_h %.17g, D_2h %.17g, tolerance %.3g\n", k,
                   adjoint[k], differences[k].d_h, differences[k].d_2h, tolerance[k]);
    }
    AADP_FUZZ_REQUIRE(std::fabs(adjoint[k] - differences[k].d_h) <= tolerance[k],
                      "adjoint differs from central difference");
  }
  return 0;
}
