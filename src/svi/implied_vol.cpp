#include "svi/implied_vol.hpp"

#include <cfloat>
#include <cmath>
#include <limits>

#include "svi/lets_be_rational_api.hpp"

namespace aadp::svi {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// Jäckel's README: |ln(F / K)| of about 700 is the natural limit of double.
constexpr double kMaxLogMoneyness = 700.0;

double Theta(OptionType type) { return type == OptionType::kCall ? 1.0 : -1.0; }

ImpliedVol Fail(InversionStatus status) { return {.vol = kNaN, .status = status}; }

ImpliedVol AtIntrinsic() { return {.vol = 0.0, .status = InversionStatus::kAtIntrinsic}; }

}  // namespace

ImpliedVol ImpliedVolatility(const Black76Quote& quote) noexcept {
  const double f = quote.forward;
  const double d = quote.discount;
  const double k = quote.strike;
  const double t = quote.expiry;
  const double v = quote.price;
  if (!std::isfinite(f) || !std::isfinite(d) || !std::isfinite(k) || !std::isfinite(t) ||
      !std::isfinite(v)) {
    return Fail(InversionStatus::kNonFiniteInput);
  }
  if (f <= 0.0) return Fail(InversionStatus::kNonPositiveForward);
  if (k <= 0.0) return Fail(InversionStatus::kNonPositiveStrike);
  if (t <= 0.0) return Fail(InversionStatus::kNonPositiveExpiry);
  if (d <= 0.0) return Fail(InversionStatus::kNonPositiveDiscount);
  if (v < 0.0) return Fail(InversionStatus::kNegativePrice);
  // F / K may overflow or underflow; the log is then infinite and fails the range test.
  if (!(std::fabs(std::log(f / k)) <= kMaxLogMoneyness)) {
    return Fail(InversionStatus::kMoneynessOutOfRange);
  }

  const bool call = quote.type == OptionType::kCall;
  const double intrinsic = call ? std::fmax(f - k, 0.0) : std::fmax(k - f, 0.0);
  const double maximum = call ? f : k;
  // The bounds are the quote's own, discounted, as the statuses say. Comparing V / D with the
  // undiscounted bounds instead adds the division's rounding, which puts a price that is at a
  // bound in double on either side of it. The intrinsic value comes first: deep in the money,
  // F - K and F (or K - F and K) can be the same double, and such a price is at intrinsic.
  const double at_intrinsic = d * intrinsic;
  if (v < at_intrinsic) return Fail(InversionStatus::kBelowIntrinsic);
  if (v == at_intrinsic) return AtIntrinsic();
  if (v >= d * maximum) return Fail(InversionStatus::kAboveMaximum);

  // Let's Be Rational works on the undiscounted price. V lies strictly between the discounted
  // bounds, but V / D can still round onto or past one of the undiscounted ones: onto the
  // intrinsic value, the time value is gone in double; onto the maximum, the price is kept just
  // below it, where the vol is finite but not attainable to any digit (C is unbounded there).
  double undiscounted = v / d;
  if (undiscounted <= intrinsic) return AtIntrinsic();
  if (undiscounted >= maximum) undiscounted = std::nextafter(maximum, 0.0);

  // T = 1 returns the total volatility s = sigma sqrt(T), so the signals stay exactly +-DBL_MAX
  // (Let's Be Rational divides them by sqrt(T) otherwise), and T is applied below. Its own,
  // normalised, comparisons can still disagree with the ones above by a rounding: below
  // intrinsic or zero means the time value is below its resolution, and above the maximum that
  // it finds no finite vol.
  const double s = ImpliedBlackVolatility(undiscounted, f, k, 1.0, Theta(quote.type));
  if (s == -DBL_MAX || s == 0.0) return AtIntrinsic();
  if (s == DBL_MAX) return Fail(InversionStatus::kNotAttained);
  if (!std::isfinite(s) || !(s > 0.0)) return Fail(InversionStatus::kNotAttained);
  const double vol = s / std::sqrt(t);
  if (!std::isfinite(vol) || !(vol > 0.0)) return Fail(InversionStatus::kNotAttained);
  if (s < DBL_MIN || vol < DBL_MIN) return Fail(InversionStatus::kVolUnderflow);
  return {.vol = vol, .status = InversionStatus::kOk};
}

double Black76Price(double forward, double discount, double strike, double expiry, double vol,
                    OptionType type) noexcept {
  if (!std::isfinite(forward) || !std::isfinite(discount) || !std::isfinite(strike) ||
      !std::isfinite(expiry) || !std::isfinite(vol)) {
    return kNaN;
  }
  if (!(forward > 0.0) || !(discount > 0.0) || !(strike > 0.0) || !(expiry > 0.0) ||
      !(vol >= 0.0)) {
    return kNaN;
  }
  return discount * Black(forward, strike, vol, expiry, Theta(type));
}

double VolConditionNumber(double forward, double strike, double expiry, double vol,
                          OptionType type) noexcept {
  if (!std::isfinite(forward) || !std::isfinite(strike) || !std::isfinite(expiry) ||
      !std::isfinite(vol) || !(forward > 0.0) || !(strike > 0.0) || !(expiry > 0.0) ||
      !(vol > 0.0)) {
    return kNaN;
  }
  const double factor =
      BlackAccuracyFactor(std::log(forward / strike), vol * std::sqrt(expiry), Theta(type));
  return 1.0 / factor;
}

const char* StatusName(InversionStatus status) noexcept {
  switch (status) {
    case InversionStatus::kOk:
      return "ok";
    case InversionStatus::kAtIntrinsic:
      return "at-intrinsic";
    case InversionStatus::kNonFiniteInput:
      return "non-finite-input";
    case InversionStatus::kNonPositiveForward:
      return "non-positive-forward";
    case InversionStatus::kNonPositiveStrike:
      return "non-positive-strike";
    case InversionStatus::kNonPositiveExpiry:
      return "non-positive-expiry";
    case InversionStatus::kNonPositiveDiscount:
      return "non-positive-discount";
    case InversionStatus::kNegativePrice:
      return "negative-price";
    case InversionStatus::kBelowIntrinsic:
      return "below-intrinsic";
    case InversionStatus::kAboveMaximum:
      return "above-maximum";
    case InversionStatus::kMoneynessOutOfRange:
      return "moneyness-out-of-range";
    case InversionStatus::kVolUnderflow:
      return "vol-underflow";
    case InversionStatus::kNotAttained:
      return "not-attained";
  }
  return "unknown";
}

}  // namespace aadp::svi
