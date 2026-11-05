// The three Let's Be Rational functions the inversion calls, declared here rather than through
// the archive's lets_be_rational.h, which pulls in dllmain.h: a global to_string, a DebugPrintf
// macro and Windows export macros. tests/svi/lets_be_rational_api_test.cpp includes both headers
// in one translation unit, so a declaration here that differs from the archive's fails to build.
//
// q is +1 for a call and -1 for a put. Signals, from lets_be_rational.cpp: an implied volatility
// of -DBL_MAX means the price is below intrinsic, +DBL_MAX that it is at or above the maximum.
#ifndef AADP_SVI_LETS_BE_RATIONAL_API_HPP_
#define AADP_SVI_LETS_BE_RATIONAL_API_HPP_

// NOLINTBEGIN(readability-identifier-naming,readability-redundant-declaration): the archive's own
// names, with C linkage, and declared twice on purpose in the test that compares them.
extern "C" {
// Black(F, K, sigma, T, q), equation (1.1) of "Let's Be Rational": the undiscounted price.
double Black(double F, double K, double sigma, double T, double q);
// The sigma with Black(F, K, sigma, T, q) = price, or a signal above.
double ImpliedBlackVolatility(double price, double F, double K, double T, double q);
// s b'(s) / b(x, s) with x = ln(F / K), s = sigma sqrt(T), b the normalised price including
// intrinsic value: the reciprocal of the conditioning of the inversion.
double BlackAccuracyFactor(double x, double s, double q);
}
// NOLINTEND(readability-identifier-naming,readability-redundant-declaration)

#endif  // AADP_SVI_LETS_BE_RATIONAL_API_HPP_
