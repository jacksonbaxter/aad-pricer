// The tape's worst case: Hogan's Lax-Wendroff advection loop, with no transcendental function.
//
// Hogan, "Fast reverse-mode automatic differentiation using expression templates in C++", ACM
// TOMS 40(4), 2014, section 6.1 and Table I: 1-D linear advection on a periodic grid of 100
// points, two of them dummies for the boundary, 2000 time steps. A tape's cost against this primal
// is the worst Hogan measured (Adept 32 to 33 times, ADOL-C 106, CppAD 214), because the primal
// loop is a few multiplies and adds that the compiler optimises freely, while every one of them
// becomes a statement on the tape (docs/NOTES.md, "AAD"; EVIDENCE B34). Written here from the
// scheme as the paper states it, not from its listing's code:
//   flux_i = (c / 2) ((q_i + q_{i+1}) + c (q_i - q_{i+1})),  i = 0..n-2,
//   q_i   += flux_{i-1} - flux_i,                            i = 1..n-2,
//   q_0 = q_{n-2}, q_{n-1} = q_1.
// Hogan does not state the Courant number; we use c = 0.5. The 100 initial values are the inputs.
// The paper seeds the 100 outputs' adjoints with a given vector; here that vector is w, and the
// scalar the bumps difference is J = sum_i w_i q_i, so one reverse sweep from J gives what Hogan's
// does. J is linear in the inputs, so a difference quotient of J has no truncation error at all:
// only round-off separates it from the gradient.
//
// The hand adjoint is the reverse of the loop above, last step first. The scheme is linear with
// constant coefficients, so the reverse pass reads nothing the forward pass computed: Hogan's hand
// adjoint costs 2.1 times the primal (1.0 + 1.1). It forms the tape's partial products, 0.5 c and
// c, in the tape's order within each flux, and adds them in another order, so tape and hand differ
// only in association.
//
// How far apart that leaves them. The association bound the pricer workloads use
// (src/ad/bounds.hpp) scales with the tape's absolute sweep, which here grows by a step's
// absolute row sum at every one of 2000 steps: it overflows to infinity and bounds nothing. So a
// stability scale instead, first order in u like the bumps' R (bench/bump_study.hpp). With
// c = 0.5 every partial is a power of two, so only additions round. One reverse step forms each
// adjoint in at most 7 roundings along any operand's path (a flux adjoint, the add inside a flux,
// up to 5 accumulations on the boundary points the copies merge), kReverseRoundings = 8; from
// terms whose absolute coefficients sum to at most 4 there (1 for the identity, 0.5 c (1 + c) and
// 0.5 c (1 - c) through each of two fluxes whose adjoints are differences of two, and 1 for the
// merged dummy), kReverseAbsoluteRowSum = 4. So one step's rounding error e has
// ||e||_2 <= gamma_8 4 ||lambda||_2 (|A| has row and column sums at most 4). Lax-Wendroff with
// c <= 1 does not amplify in L2 (von Neumann, |g| <= 1), so neither the adjoint, ||lambda||_2 <=
// ||w||_2, nor an error once made grows over later steps: per pass, at most
// kTimeSteps gamma_8 4 ||w||_2, which bounds each entry too. Twice that between hand and tape.
// First order: it leaves out errors on errors, and the copies' first fold of the dummies' seeds
// (bench/aad_checks.cpp, CheckHandLw).
#ifndef AADP_BENCH_LAX_WENDROFF_HPP_
#define AADP_BENCH_LAX_WENDROFF_HPP_

#include <array>
#include <cstddef>
#include <cstdint>

#include "math/exp.hpp"

namespace aadp::bench::lw {

inline constexpr std::size_t kPoints = 100;
inline constexpr std::uint32_t kTimeSteps = 2000;
inline constexpr double kCourant = 0.5;

// The hand-against-tape scale above, for c = 0.5.
inline constexpr double kReverseRoundings = 8.0;
inline constexpr double kReverseAbsoluteRowSum = 4.0;
inline constexpr double kHandTapePasses = 2.0;

template <class Real>
using Field = std::array<Real, kPoints>;

// A smooth bump on a level of one, on the interior points, with the boundary dummies set as the
// scheme sets them. Its values do not change the cost: the scheme is linear.
inline Field<double> InitialField() {
  Field<double> q{};
  for (std::size_t i = 1; i + 1 < kPoints; ++i) {
    const double x = (static_cast<double>(i) - 50.0) / 10.0;
    q[i] = 1.0 + math::exp(-x * x);
  }
  q[0] = q[kPoints - 2];
  q[kPoints - 1] = q[1];
  return q;
}

// Output weights: a ramp, so that no weighted sum is conserved by the scheme and the gradient is
// not trivial. A constant weight would give J = the conserved mass.
inline Field<double> OutputWeights() {
  Field<double> w{};
  for (std::size_t i = 0; i < kPoints; ++i) {
    w[i] = static_cast<double>(i + 1) / static_cast<double>(kPoints);
  }
  return w;
}

// Advances q in place. Real is double or ad::Real.
template <class Real>
void Advect(Field<Real>& q, std::uint32_t steps, double c) {
  const double half_c = 0.5 * c;
  std::array<Real, kPoints - 1> flux{};
  for (std::uint32_t step = 0; step < steps; ++step) {
    for (std::size_t i = 0; i + 1 < kPoints; ++i) {
      // Named in clang's order, so the tape's statement order does not depend on the compiler
      // (src/pricer/kernels.hpp, Finish).
      const Real sum = q[i] + q[i + 1];
      const Real difference = q[i] - q[i + 1];
      flux[i] = half_c * (sum + c * difference);
    }
    for (std::size_t i = 1; i + 1 < kPoints; ++i) {
      q[i] = q[i] + (flux[i - 1] - flux[i]);
    }
    q[0] = q[kPoints - 2];
    q[kPoints - 1] = q[1];
  }
}

template <class Real>
Real Functional(const Field<Real>& q, const Field<double>& w) {
  Real sum = w[0] * q[0];
  for (std::size_t i = 1; i < kPoints; ++i) sum = sum + w[i] * q[i];
  return sum;
}

// The primal: J of the advected field.
inline double Primal(const Field<double>& q_init, const Field<double>& w) {
  Field<double> q = q_init;
  Advect(q, kTimeSteps, kCourant);
  return Functional(q, w);
}

// The hand adjoint's reverse pass on n points, `steps` steps last first, in place: qbar holds the
// outputs' adjoints on entry and the inputs' on exit. Gather form: each adjoint is written once a
// step, as one sum, so no iteration updates a neighbour's adjoint in place and the loops
// vectorise. The sums are those of the scatter form it replaced (a loop over the points that
// added each point's adjoint into two flux adjoints, then each flux adjoint into two point
// adjoints), in its order: the flux adjoint of points i and i+1 is (0 + qbar_{i+1}) - qbar_i, and
// a point's adjoint is (qbar_j + up_j) + down_{j-1}, up and down the two halves of the flux
// adjoints beside it. The zeros the scatter form started from are added too, so a zero keeps its
// sign; tests/bench/lax_wendroff_test.cpp holds the scatter form and checks the bits.
template <std::size_t N>
void ReverseSteps(std::array<double, N>& qbar, std::uint32_t steps, double c) {
  static_assert(N >= 3, "the scheme needs an interior point");
  const double half_c = 0.5 * c;
  std::array<double, N - 1> fbar{};
  // A flux adjoint's two contributions: to the point on its left and to the one on its right.
  const auto up = [&](std::size_t i) {
    const double sum_bar = half_c * fbar[i];
    return sum_bar + (c * sum_bar);
  };
  const auto down = [&](std::size_t i) {
    const double sum_bar = half_c * fbar[i];
    return sum_bar - (c * sum_bar);
  };
  for (std::uint32_t step = 0; step < steps; ++step) {
    // q_{n-1} = q_1, then q_0 = q_{n-2}, undone last first.
    qbar[1] += qbar[N - 1];
    qbar[N - 1] = 0.0;
    qbar[N - 2] += qbar[0];
    qbar[0] = 0.0;
    fbar[0] = 0.0 + qbar[1];
    for (std::size_t i = 1; i + 2 < N; ++i) fbar[i] = (0.0 + qbar[i + 1]) - qbar[i];
    fbar[N - 2] = 0.0 - qbar[N - 2];
    qbar[0] = 0.0 + up(0);
    for (std::size_t j = 1; j + 1 < N; ++j) qbar[j] = (qbar[j] + up(j)) + down(j - 1);
    qbar[N - 1] = 0.0 + down(N - 2);
  }
}

// The hand adjoint: J, and dJ/dq_init in gradient. Forward pass, then the reverse pass.
inline double HandGradient(const Field<double>& q_init, const Field<double>& w,
                           Field<double>& gradient) {
  const double value = Primal(q_init, w);
  gradient = w;
  ReverseSteps(gradient, kTimeSteps, kCourant);
  return value;
}

}  // namespace aadp::bench::lw

#endif  // AADP_BENCH_LAX_WENDROFF_HPP_
