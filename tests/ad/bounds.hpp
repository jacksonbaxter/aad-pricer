// The round-off bound every tape check states before it measures.
//
// Tape and Dual compute the same pathwise derivative, sum over paths p from an input to the
// output of the product of the partials along p, from the same partials: real.hpp writes each
// partial exactly as tests/pricer/dual_ops.hpp does, on the same values, so the partials are the
// same doubles. They differ only in association. Each term picks up one rounding per product and
// per addition it passes through (no FMA: the build has -ffp-contract=off):
//   reverse  adj_j += adj_i d_ij: a term through a path of l statements passes l products and,
//            at each statement, at most its fan-out of additions, so at most T + O roundings,
//            with T statements and O operands on the tape;
//   forward  t_i = sum_j d_ij t_j: at most one product and one addition per statement, 2 T.
// So each is within gamma_n A of the exact sum (Higham, Accuracy and Stability of Numerical
// Algorithms, 2nd ed., lemma 3.1 and section 3.1), with n = 2 T + O covering both, and A the sum
// of |terms|: the same sweep on |partials|, which the tape computes (SweepBackAbsolute) with
// relative error at most gamma_n.
//
// That model, fl(x op y) = (x op y)(1 + delta) with |delta| <= u, holds only in the normal range.
// With gradual underflow it becomes fl(x op y) = (x op y)(1 + delta) + eta, |eta| <= 2^-1075,
// half the smallest subnormal, and eta = 0 for addition and subtraction, which are exact when
// their result is subnormal (Higham, section 2.2, underflow). So only products add an absolute
// error, and each operand on the tape is one product in either sweep:
//   reverse  adj_i d_ij puts eta into adj_j, which reaches input k's adjoint times at most
//            Tbar_j, the absolute tangent of j seeded on input k (SweepForwardAbsolute);
//   forward  d_ij t_j puts eta into t_i, which reaches the output times at most Abar_i, the
//            absolute adjoint of i (SweepBackAbsolute).
// The computed A carries the reverse term too. Tbar and Abar are computed to within gamma_n and
// terms of order 2^-2150. With W_k the sum over operands of Abar of the statement plus Tbar of
// the operand (Tape::SumOverOperands, PathResult::operand_sums), per input k,
//   |tape - dual| <= 2 gamma_n A (1 + 2 gamma_n) + 2^-1074 W_k (1 + 2 gamma_n) + 2^-1072,
// 2^-1074 being 2^-1075 twice, for the tape's reverse term and A's, and 2^-1072 the rounding of
// the bound's own operations. This replaces n 2^-1070, which counted each loss once and not its
// amplification, and failed below the normal range (10.8 times, OneStepCorner, p near 1e-316;
// 45.8 times, a digital path worth 6e-319 in the pricer-gradient fuzzer).
#ifndef AADP_TESTS_AD_BOUNDS_HPP_
#define AADP_TESTS_AD_BOUNDS_HPP_

#include <cstddef>

namespace aadp::ad::testing {

inline constexpr double kUnitRoundoff = 0x1p-53;

inline double Gamma(double n) {
  const double nu = n * kUnitRoundoff;
  return nu / (1.0 - nu);
}

inline double RoundoffTerms(std::size_t statements, std::size_t operands) {
  return (2.0 * static_cast<double>(statements)) + static_cast<double>(operands);
}

// 2^-1075 per product counted twice (see above), times the weight W of the products.
inline double UnderflowTerm(double weight) { return weight * 0x1p-1074; }

inline constexpr double kBoundRounding = 0x1p-1072;

inline double AgreementBound(double absolute, double operand_sums, std::size_t statements,
                             std::size_t operands) {
  const double n = RoundoffTerms(statements, operands);
  const double gamma = Gamma(n);
  return (2.0 * gamma * absolute * (1.0 + (2.0 * gamma))) +
         (UnderflowTerm(operand_sums) * (1.0 + (2.0 * gamma))) + kBoundRounding;
}

}  // namespace aadp::ad::testing

#endif  // AADP_TESTS_AD_BOUNDS_HPP_
