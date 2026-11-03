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
// relative error at most gamma_n. Hence, per input,
//   |tape - dual| <= 2 gamma_n A (1 + 2 gamma_n) + n 2^-1070,
// the last term for products that fall below the normal range (each loses at most 2^-1075). That
// term assumes nothing downstream amplifies such a loss by more than 32. A path whose own values
// are subnormal breaks that assumption, and the bound does not hold there: OneStepCorner's second
// test measured 10.8 times it at a step with p near 1e-316.
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

inline double AgreementBound(double absolute, std::size_t statements, std::size_t operands) {
  const double n = RoundoffTerms(statements, operands);
  const double gamma = Gamma(n);
  return (2.0 * gamma * absolute * (1.0 + (2.0 * gamma))) + (n * 0x1p-1070);
}

}  // namespace aadp::ad::testing

#endif  // AADP_TESTS_AD_BOUNDS_HPP_
