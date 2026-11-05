// SVI-1: Black-76 implied volatility through Let's Be Rational, at the precision IEEE double can
// attain for each quote.
//
// The tolerance was stated in NOTES.md ("Stage 6a", "SVI-1 tolerance") before this test first
// ran, with its reason. For a quote priced at sigma and inverted to sigma-hat:
//   vol    |sigma-hat / sigma - 1|   <= 8 eta (1 + C)
//   price  |V(sigma-hat) / V - 1|    <= 8 eta (1 + 1 / C)
// with C = V / (sigma dV/dsigma), eta = eps + 2^-1074 / V. Where 8 eta (1 + C) >= 1 no digit of
// sigma is attainable, and those points are counted, not hidden. A point at its intrinsic value in
// double must return at-intrinsic with vol 0 and is judged by that and the price bound (JudgeVol
// says why; NOTES.md has the first run, which applied the vol bound there too).
//
// The grid: T in {0.0137, 0.0274, 0.0833, 0.25, 0.5, 1, 2, 5}, sigma in {0.01, 0.03, 0.1, 0.2,
// 0.4, 0.8, 1.6, 3.2}, z = ln(K/F) / (sigma sqrt(T)) from -36 to 36 by 0.5, calls and puts,
// F = 100, D = exp(-0.03 T): 18,560 quotes. It prints the worst cases.
//
// The round trip cannot see a fault that the price and the inversion share, such as a swapped
// call/put flag, so the price is also checked against the oracle's Black-Scholes closed form.
#include "svi/implied_vol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "oracle/black_scholes.hpp"
#include "rng/normal.hpp"

namespace aadp::svi {
namespace {

constexpr double kEps = 0x1p-52;
constexpr double kTiny = 0x1p-1074;
constexpr double kMinNormal = std::numeric_limits<double>::min();
constexpr double kFactor = 8.0;
constexpr double kForward = 100.0;
constexpr double kRate = 0.03;
constexpr std::array<double, 8> kExpiries{0.0137, 0.0274, 0.0833, 0.25, 0.5, 1.0, 2.0, 5.0};
constexpr std::array<double, 8> kVols{0.01, 0.03, 0.1, 0.2, 0.4, 0.8, 1.6, 3.2};
constexpr double kZMax = 36.0;
constexpr double kZStep = 0.5;
constexpr int kZCount = 145;
// The oracle comparison: 2^-36 of the terms' magnitude, on |z| <= 6 and s <= 3 (NOTES.md).
constexpr double kOracleRelative = 0x1p-36;
constexpr double kOracleZ = 6.0;
constexpr double kOracleS = 3.0;

const char* TypeName(OptionType type) { return type == OptionType::kCall ? "call" : "put"; }

struct GridPoint {
  double expiry = 0.0;
  double discount = 1.0;
  double vol = 0.0;
  double total = 0.0;  // s = sigma sqrt(T)
  double z = 0.0;
  double strike = 0.0;
  OptionType type = OptionType::kCall;
};

std::vector<GridPoint> Grid() {
  std::vector<GridPoint> grid;
  for (const double t : kExpiries) {
    for (const double sigma : kVols) {
      const double total = sigma * std::sqrt(t);
      for (int i = 0; i < kZCount; ++i) {
        const double z = -kZMax + (kZStep * static_cast<double>(i));
        for (const OptionType type : {OptionType::kCall, OptionType::kPut}) {
          grid.push_back({.expiry = t,
                          .discount = std::exp(-kRate * t),
                          .vol = sigma,
                          .total = total,
                          .z = z,
                          .strike = kForward * std::exp(z * total),
                          .type = type});
        }
      }
    }
  }
  return grid;
}

std::string Where(const GridPoint& g, double price) {
  std::array<char, 160> text{};
  std::snprintf(text.data(), text.size(), "z %+.1f sigma %g T %g %s price %.17g", g.z, g.vol,
                g.expiry, TypeName(g.type), price);
  return text.data();
}

struct Worst {
  double ratio = 0.0;  // error / bound
  double error = 0.0;
  double condition = 0.0;
  GridPoint at;

  void Offer(double candidate_ratio, double candidate_error, double c, const GridPoint& g) {
    if (!(candidate_ratio > ratio)) return;
    ratio = candidate_ratio;
    error = candidate_error;
    condition = c;
    at = g;
  }

  void Print(const char* what) const {
    std::printf("%s: ratio %.4g, error %.4g (%.3g ulps), C %.4g, at z %+.1f sigma %g T %g %s\n",
                what, ratio, error, error / kEps, condition, at.z, at.vol, at.expiry,
                TypeName(at.type));
  }
};

struct Survey {
  std::size_t quotes = 0;
  std::size_t ok = 0;
  std::size_t at_intrinsic = 0;
  std::size_t other_status = 0;
  std::size_t vacuous = 0;  // 8 eta (1 + C) >= 1
  std::size_t vol_failures = 0;
  std::size_t price_failures = 0;
  double min_otm_price = std::numeric_limits<double>::infinity();
  Worst vol;              // by ratio to the bound, points with a bound below 1
  Worst price;            // by ratio to the bound
  Worst vol_ulps_well;    // by raw error, ok points with C <= 1 and a normal price
  Worst price_ulps_well;  // by raw error, points with 1 / C <= 1 and a normal price
  std::string first_failure;
  std::vector<std::string> failures;  // every failing point, printed by the test
  // At-intrinsic points where 8 eta (1 + C) < 1 all the same; see JudgeVol.
  std::vector<std::string> flat_with_bound;
  std::array<std::size_t, 16> by_status{};

  void Fail(const std::string& what) {
    if (first_failure.empty()) first_failure = what;
    failures.push_back(what);
  }
};

double Intrinsic(const GridPoint& g) {
  return g.type == OptionType::kCall ? std::fmax(kForward - g.strike, 0.0)
                                     : std::fmax(g.strike - kForward, 0.0);
}

// Counts the status; false if the point has no vol to judge.
bool JudgeStatus(Survey& s, const GridPoint& g, double price, const ImpliedVol& iv) {
  if (iv.status == InversionStatus::kAtIntrinsic) {
    ++s.at_intrinsic;
    // Allowed only where the price really carries no time value in double: it is D times the
    // intrinsic value, or undiscounting it leaves the intrinsic value.
    if (price != g.discount * Intrinsic(g) && price / g.discount > Intrinsic(g)) {
      s.Fail("at-intrinsic where the price is not: " + Where(g, price));
    }
    return true;
  }
  if (iv.status == InversionStatus::kOk) {
    ++s.ok;
    return true;
  }
  ++s.other_status;
  s.Fail(std::string(StatusName(iv.status)) + " at " + Where(g, price));
  return false;
}

// At an at-intrinsic point the price is flat in sigma in double: every sigma up to some bound,
// the quote's own among them, gives the same double, so no vol is attainable to any digit and
// NOTES' status rule asks for vol 0. The vol bound, a linearisation, is not applied there; the
// points where it would not be vacuous are counted and listed.
void JudgeVol(Survey& s, const GridPoint& g, double price, const ImpliedVol& iv, double c,
              double vol_bound) {
  const double vol_error = std::fabs((iv.vol / g.vol) - 1.0);
  if (iv.status == InversionStatus::kAtIntrinsic) {
    if (vol_bound < 1.0) {
      std::array<char, 80> text{};
      std::snprintf(text.data(), text.size(), "bound %.4g, C %.4g, at ", vol_bound, c);
      s.flat_with_bound.push_back(text.data() + Where(g, price));
    }
    return;
  }
  if (!(vol_error <= vol_bound)) {
    ++s.vol_failures;
    std::array<char, 200> text{};
    std::snprintf(text.data(), text.size(), "vol error %.4g over bound %.4g at ", vol_error,
                  vol_bound);
    s.Fail(text.data() + Where(g, price));
  }
  if (vol_bound < 1.0) s.vol.Offer(vol_error / vol_bound, vol_error, c, g);
  if (c <= 1.0 && price >= kMinNormal) s.vol_ulps_well.Offer(vol_error, vol_error, c, g);
}

void JudgePoint(Survey& s, const GridPoint& g) {
  ++s.quotes;
  const double price = Black76Price(kForward, g.discount, g.strike, g.expiry, g.vol, g.type);
  const bool otm = (g.type == OptionType::kCall) == (g.strike > kForward);
  if (otm && price > 0.0) s.min_otm_price = std::min(s.min_otm_price, price);
  const ImpliedVol iv = ImpliedVolatility({.forward = kForward,
                                           .discount = g.discount,
                                           .strike = g.strike,
                                           .expiry = g.expiry,
                                           .price = price,
                                           .type = g.type});
  ++s.by_status.at(static_cast<std::size_t>(iv.status));
  if (!JudgeStatus(s, g, price, iv)) return;

  const double c = VolConditionNumber(kForward, g.strike, g.expiry, g.vol, g.type);
  const double eta = kEps + (kTiny / price);
  const double vol_bound = kFactor * eta * (1.0 + c);
  const double price_bound = kFactor * eta * (1.0 + (1.0 / c));
  std::array<char, 200> text{};

  if (vol_bound >= 1.0) ++s.vacuous;
  JudgeVol(s, g, price, iv, c, vol_bound);

  const double repriced = Black76Price(kForward, g.discount, g.strike, g.expiry, iv.vol, g.type);
  const double price_error = price > 0.0 ? std::fabs((repriced / price) - 1.0) : 0.0;
  if (!(price_error <= price_bound)) {
    ++s.price_failures;
    std::snprintf(text.data(), text.size(), "price error %.4g over bound %.4g at ", price_error,
                  price_bound);
    s.Fail(text.data() + Where(g, price));
  }
  s.price.Offer(price_error / price_bound, price_error, c, g);
  if (c >= 1.0 && price >= kMinNormal) {
    s.price_ulps_well.Offer(price_error, price_error, c, g);
  }
}

TEST(ImpliedVol, RoundTripsAtTheAttainablePrecisionOverTheStatedGrid) {
  Survey s;
  for (const GridPoint& g : Grid()) JudgePoint(s, g);
  std::printf(
      "SVI-1 grid: %zu quotes, %zu ok, %zu at-intrinsic, %zu other, %zu with a vacuous "
      "vol bound (8 eta (1 + C) >= 1)\n",
      s.quotes, s.ok, s.at_intrinsic, s.other_status, s.vacuous);
  std::printf("SVI-1 smallest out-of-the-money price on the grid: %.4g\n", s.min_otm_price);
  s.vol.Print("SVI-1 worst vol, by ratio to its bound");
  s.price.Print("SVI-1 worst price, by ratio to its bound");
  s.vol_ulps_well.Print("SVI-1 worst vol where C <= 1, raw");
  s.price_ulps_well.Print("SVI-1 worst price where C >= 1, raw");
  for (std::size_t i = 0; i < s.by_status.size(); ++i) {
    if (s.by_status.at(i) == 0) continue;
    std::printf("SVI-1 status %s: %zu\n", StatusName(static_cast<InversionStatus>(i)),
                s.by_status.at(i));
  }
  for (const std::string& f : s.failures) std::printf("SVI-1 failure: %s\n", f.c_str());
  std::printf("SVI-1 at-intrinsic points with a vol bound below 1, not applied: %zu\n",
              s.flat_with_bound.size());
  for (const std::string& f : s.flat_with_bound) std::printf("SVI-1   %s\n", f.c_str());
  EXPECT_EQ(s.quotes, std::size_t{18560});
  EXPECT_EQ(s.other_status, 0U);
  EXPECT_EQ(s.vol_failures, 0U);
  EXPECT_EQ(s.price_failures, 0U);
  EXPECT_TRUE(s.first_failure.empty()) << s.first_failure;
}

// |Black76Price - BlackScholes| / (D (F N(d1) + K N(d2))), signs by type.
double OracleDifference(const GridPoint& g) {
  const double rate = -std::log(g.discount) / g.expiry;
  const double d1 = (std::log(kForward / g.strike) + (0.5 * g.total * g.total)) / g.total;
  const double d2 = d1 - g.total;
  const bool call = g.type == OptionType::kCall;
  const double phi = call ? 1.0 : -1.0;
  const double scale =
      g.discount * ((kForward * normal_cdf(phi * d1)) + (g.strike * normal_cdf(phi * d2)));
  const MarketParams<double> market{
      .spot = kForward, .sigma = g.vol, .r = rate, .q = rate, .maturity = g.expiry};
  const double reference =
      BlackScholes(call ? aadp::OptionType::kCall : aadp::OptionType::kPut, g.strike, market).value;
  const double price = Black76Price(kForward, g.discount, g.strike, g.expiry, g.vol, g.type);
  return std::fabs(price - reference) / scale;
}

TEST(ImpliedVol, PriceMatchesTheBlackScholesOracle) {
  std::size_t compared = 0;
  double worst = 0.0;
  for (const GridPoint& g : Grid()) {
    if (g.total > kOracleS || std::fabs(g.z) > kOracleZ) continue;
    const double difference = OracleDifference(g);
    worst = std::max(worst, difference);
    EXPECT_LE(difference, kOracleRelative)
        << "z " << g.z << " sigma " << g.vol << " T " << g.expiry << " " << TypeName(g.type);
    ++compared;
  }
  std::printf("SVI-1 oracle: %zu prices, worst difference %.3g of the terms (bound %.3g)\n",
              compared, worst, kOracleRelative);
  EXPECT_GT(compared, 1000U);
}

TEST(ImpliedVol, InvalidQuotesGetNamedErrorValuesAndNaN) {
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();
  const Black76Quote good{.forward = 100.0,
                          .discount = 0.99,
                          .strike = 110.0,
                          .expiry = 0.5,
                          .price = 2.0,
                          .type = OptionType::kCall};
  ASSERT_EQ(ImpliedVolatility(good).status, InversionStatus::kOk);
  struct Case {
    Black76Quote quote;
    InversionStatus status = InversionStatus::kOk;
  };
  const auto with = [&good](auto change) {
    Black76Quote q = good;
    change(q);
    return q;
  };
  using enum InversionStatus;
  const std::vector<Case> cases{
      {.quote = with([](Black76Quote& q) { q.forward = kNaN; }), .status = kNonFiniteInput},
      {.quote = with([](Black76Quote& q) { q.price = kInf; }), .status = kNonFiniteInput},
      {.quote = with([](Black76Quote& q) { q.discount = -kInf; }), .status = kNonFiniteInput},
      {.quote = with([](Black76Quote& q) { q.expiry = kNaN; }), .status = kNonFiniteInput},
      {.quote = with([](Black76Quote& q) { q.strike = kInf; }), .status = kNonFiniteInput},
      {.quote = with([](Black76Quote& q) { q.forward = 0.0; }), .status = kNonPositiveForward},
      {.quote = with([](Black76Quote& q) { q.strike = -1.0; }), .status = kNonPositiveStrike},
      {.quote = with([](Black76Quote& q) { q.expiry = 0.0; }), .status = kNonPositiveExpiry},
      {.quote = with([](Black76Quote& q) { q.discount = 0.0; }), .status = kNonPositiveDiscount},
      {.quote = with([](Black76Quote& q) { q.price = -1e-300; }), .status = kNegativePrice},
      // A call is worth less than D F, a put less than D K.
      {.quote = with([](Black76Quote& q) { q.price = 99.0; }), .status = kAboveMaximum},
      {.quote = with([](Black76Quote& q) {
         q.type = OptionType::kPut;
         q.price = 0.99 * 110.0;
       }),
       .status = kAboveMaximum},
      // Below the discounted intrinsic value: an in-the-money put, D (K - F) = 9.9.
      {.quote = with([](Black76Quote& q) {
         q.type = OptionType::kPut;
         q.price = 9.8;
       }),
       .status = kBelowIntrinsic},
      {.quote = with([](Black76Quote& q) {
         q.strike = 1e-306;
         q.price = 50.0;
       }),
       .status = kMoneynessOutOfRange},
      {.quote = with([](Black76Quote& q) { q.price = 0.0; }), .status = kAtIntrinsic},
      // At the money, V = D F erf(s / sqrt(8)), about 0.4 D F s, so V = 4.9e-307 is s = 1.2e-308,
      // below DBL_MIN, and sigma = s / 2^32 lower still. The fuzzer found it at T = 2^64 years.
      {.quote = with([](Black76Quote& q) {
         q.strike = 100.0;
         q.expiry = 0x1p64;
         q.price = 4.9e-307;
       }),
       .status = kVolUnderflow},
  };
  for (const Case& c : cases) {
    const ImpliedVol iv = ImpliedVolatility(c.quote);
    EXPECT_EQ(iv.status, c.status)
        << StatusName(iv.status) << " instead of " << StatusName(c.status);
    if (c.status == kAtIntrinsic) {
      EXPECT_EQ(iv.vol, 0.0);
    } else {
      EXPECT_TRUE(std::isnan(iv.vol)) << StatusName(c.status);
    }
  }
}

TEST(ImpliedVol, BoundsAreTheDiscountedOnesNotTheUndiscounted) {
  // An in-the-money call between D (F - K) and F - K is a valid quote; one just under D (F - K)
  // is not.
  const double discount = 0.9;
  const Black76Quote above{.forward = 100.0,
                           .discount = discount,
                           .strike = 80.0,
                           .expiry = 1.0,
                           .price = 18.5,
                           .type = OptionType::kCall};
  const ImpliedVol iv = ImpliedVolatility(above);
  EXPECT_EQ(iv.status, InversionStatus::kOk) << StatusName(iv.status);
  Black76Quote below = above;
  below.price = 17.9;
  EXPECT_EQ(ImpliedVolatility(below).status, InversionStatus::kBelowIntrinsic);
  // And a call just under D F, above F - K's discounted value, inverts.
  Black76Quote near_cap = above;
  near_cap.price = (discount * 100.0) - 1e-6;
  EXPECT_EQ(ImpliedVolatility(near_cap).status, InversionStatus::kOk);
}

TEST(ImpliedVol, StatusNamesAreDistinct) {
  using enum InversionStatus;
  constexpr std::array kAll{kOk,
                            kAtIntrinsic,
                            kNonFiniteInput,
                            kNonPositiveForward,
                            kNonPositiveStrike,
                            kNonPositiveExpiry,
                            kNonPositiveDiscount,
                            kNegativePrice,
                            kBelowIntrinsic,
                            kAboveMaximum,
                            kMoneynessOutOfRange,
                            kVolUnderflow,
                            kNotAttained};
  for (const InversionStatus a : kAll) {
    for (const InversionStatus b : kAll) {
      if (a != b) {
        EXPECT_STRNE(StatusName(a), StatusName(b));
      }
    }
    EXPECT_STRNE(StatusName(a), "unknown");
  }
}

}  // namespace
}  // namespace aadp::svi
