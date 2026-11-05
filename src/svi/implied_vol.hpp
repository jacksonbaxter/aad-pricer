// Black-76 implied volatility through Peter Jäckel's "Let's Be Rational" (revision 1520, fetched
// by cmake/AadpDependencies.cmake; licence in NOTICE).
//
// A quote is a forward F, a discount factor D, a strike K, a time to expiry T in years and a
// discounted price V of a call or a put. Black-76 prices it as
//   V = D (F N(d1) - K N(d2)) for a call, D (K N(-d2) - F N(-d1)) for a put,
//   d1,2 = (ln(F / K) +- s^2 / 2) / s,  s = sigma sqrt(T).
// ImpliedVolatility returns the sigma that gives V, or an error value. It never throws and never
// aborts: every input, NaN and infinities included, gets a status.
//
// Accuracy (SVI-1). The inversion is judged against what IEEE double can attain, which depends on
// the conditioning of the quote, C = V / (sigma dV/dsigma): a relative error e in V moves sigma by
// about C e. VolConditionNumber returns C, from Jäckel's BlackAccuracyFactor, which evaluates it in
// scaled form so that it does not underflow in the wings. The tolerance and its reason are in
// NOTES.md, "Stage 6a", and tests/svi/implied_vol_test.cpp measures it.
#ifndef AADP_SVI_IMPLIED_VOL_HPP_
#define AADP_SVI_IMPLIED_VOL_HPP_

#include <cstdint>

namespace aadp::svi {

enum class OptionType : std::uint8_t { kCall, kPut };

enum class InversionStatus : std::uint8_t {
  kOk,
  // The price carries no time value in double: it is the discounted intrinsic value exactly, or
  // its time value is lost in undiscounting it or below Let's Be Rational's resolution. Every
  // small enough sigma gives the same double, so 0 is returned, and it says nothing about
  // volatility.
  kAtIntrinsic,
  kNonFiniteInput,       // F, D, K, T or V is NaN or infinite.
  kNonPositiveForward,   // F <= 0
  kNonPositiveStrike,    // K <= 0
  kNonPositiveExpiry,    // T <= 0
  kNonPositiveDiscount,  // D <= 0
  kNegativePrice,        // V < 0
  kBelowIntrinsic,       // V < D max(F - K, 0) for a call, D max(K - F, 0) for a put
  kAboveMaximum,         // V >= D F for a call, D K for a put: no finite sigma reaches it
  // |ln(F / K)| > 700, beyond which Jäckel's README says the normalised prices are not
  // representable in double.
  kMoneynessOutOfRange,
  // sigma, or s = sigma sqrt(T), is below DBL_MIN: subnormal, so it would carry fewer than 53
  // bits, and no price computed from it could be trusted to double precision. Takes a tiny
  // price at an absurd expiry, such as T = 1e19 years at the money.
  kVolUnderflow,
  // Let's Be Rational returned something that is not a positive finite volatility for a quote
  // that passed every check above, such as its above-maximum signal for a price within a
  // rounding of the maximum. It exists so that such a result cannot pass as a volatility.
  kNotAttained,
};

struct Black76Quote {
  double forward = 0.0;
  double discount = 1.0;
  double strike = 0.0;
  double expiry = 0.0;
  double price = 0.0;
  OptionType type = OptionType::kCall;
};

struct ImpliedVol {
  double vol = 0.0;  // NaN unless status is kOk or kAtIntrinsic.
  InversionStatus status = InversionStatus::kNotAttained;
};

[[nodiscard]] ImpliedVol ImpliedVolatility(const Black76Quote& quote) noexcept;

// The discounted Black-76 price, Let's Be Rational's Black() times D. NaN when an input is not
// finite or F, K, T or D is not positive, or vol is negative.
[[nodiscard]] double Black76Price(double forward, double discount, double strike, double expiry,
                                  double vol, OptionType type) noexcept;

// C = V / (sigma dV/dsigma) for the quote at vol, with V the full price (intrinsic included), so
// the conditioning of the price as quoted. NaN under the same conditions as Black76Price, and
// for vol = 0.
[[nodiscard]] double VolConditionNumber(double forward, double strike, double expiry, double vol,
                                        OptionType type) noexcept;

[[nodiscard]] const char* StatusName(InversionStatus status) noexcept;

}  // namespace aadp::svi

#endif  // AADP_SVI_IMPLIED_VOL_HPP_
