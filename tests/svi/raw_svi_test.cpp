// Raw SVI (src/svi/raw_svi.hpp): w, w', w'', g and d+-, in double and on the tape.
//
// Checks, each with its tolerance stated here before it ran:
// 1. On the tape (aadp::ad::Real), every value is the double value bit for bit: the template
//    performs the same operations in the same order.
// 2. The tape's dw/dk is w' and its dw'/dk is w'', within 8 eps times the sum of the magnitudes
//    of the terms the chain rule adds (b (|rho| + |x| / r) for w', b (1/r + x^2/r^3) for w''):
//    the two differ only in how the same products are rounded and summed, and dw'/dk subtracts
//    two terms near 1/r that cancel down to sigma^2 / r^3 far from m.
// 3. g against the density from second differences of Black call prices, through Martini and
//    Mingone, arXiv 2005.03340, eq. (1): d^2 C / dK^2 = g(k) phi(d-(k)) / (K sqrt(w(k))) with F =
//    1, T = 1. Central differences at h = 1e-3 K and h / 2, Richardson-extrapolated, leave a
//    truncation error near h^4 C^(6) / 6!, about 1e-9 here, and a rounding error near 16 eps V /
//    (h/2)^2 times K sqrt(w) / phi, about 1e-9 too, with V the out-of-the-money price (put below
//    the forward, call above; their second differences agree by parity). An in-the-money call
//    would carry its intrinsic value's rounding, up to 1e-4 in g at k = -2. The tolerance on g is
//    1e-6, a margin of about 1000 over both, while Vogt's g reaches -0.033.
// 4. d+ as k -> +infinity: to -infinity if b (1 + rho) < 2, to 0 at 2, to +infinity above (MM
//    Lemma 2.1). With w ~ beta k, d+ ~ (sqrt(beta) / 2 - 1 / sqrt(beta)) sqrt(k), about -0.075
//    sqrt(k) at beta = 1.8 and 0.083 sqrt(k) at 2.25, so the sign and growth show at k = 1e8.
#include "svi/raw_svi.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "ad/real.hpp"  // IWYU pragma: keep (the operations raw_svi.hpp applies to ad::Real)
#include "ad/tape.hpp"
#include "svi/implied_vol.hpp"

namespace aadp::svi {
namespace {

constexpr double kEps = 0x1p-52;

using Params = RawSvi<double>;

// Gatheral and Jacquier, arXiv 1204.0646, eq. (3.8): Axel Vogt's slice, t = 1.
constexpr Params kVogt{.a = -0.0410, .b = 0.1331, .rho = 0.3060, .m = 0.3586, .sigma = 0.4153};
// Martini and Mingone, Table 1, rows 0 and 2: arbitrage-free.
constexpr Params kMm0{.a = 0.10, .b = 1.0, .rho = -0.306, .m = 0.10, .sigma = 0.30};
constexpr Params kMm2{.a = 0.01, .b = 0.1, .rho = -0.600, .m = -0.05, .sigma = 0.10};

constexpr std::array<Params, 3> kSlices{kVogt, kMm0, kMm2};
constexpr std::array<double, 9> kPoints{-2.0, -0.7, -0.1, 0.0, 0.05, 0.3586, 0.879, 1.5, 3.0};

RawSvi<ad::Real> Passive(const Params& p) {
  return {.a = ad::Real(p.a),
          .b = ad::Real(p.b),
          .rho = ad::Real(p.rho),
          .m = ad::Real(p.m),
          .sigma = ad::Real(p.sigma)};
}

TEST(RawSvi, OnTheTapeEveryValueIsTheDoubleValueBitForBit) {
  for (const Params& p : kSlices) {
    for (const double k : kPoints) {
      ad::Tape tape;
      const ad::TapeScope scope(tape);
      const ad::Real x = tape.Register(k);
      const RawSvi<ad::Real> q = Passive(p);
      const SviPoint<ad::Real> s = Evaluate(q, x);
      const SviPoint<double> d = Evaluate(p, k);
      EXPECT_EQ(s.w.value(), d.w);
      EXPECT_EQ(s.w1.value(), d.w1);
      EXPECT_EQ(s.w2.value(), d.w2);
      EXPECT_EQ(TotalVariance(q, x).value(), TotalVariance(p, k));
      EXPECT_EQ(Durrleman(q, x).value(), Durrleman(p, k));
      EXPECT_EQ(DPlus(s.w, x).value(), DPlus(d.w, k));
      EXPECT_EQ(DMinus(s.w, x).value(), DMinus(d.w, k));
    }
  }
}

TEST(RawSvi, TapeDerivativesInKAreTheClosedFormWPrimeAndWSecond) {
  for (const Params& p : kSlices) {
    for (const double k : kPoints) {
      const SviPoint<double> d = Evaluate(p, k);
      const double x = k - p.m;
      const double r = std::sqrt((x * x) + (p.sigma * p.sigma));
      {
        ad::Tape tape;
        const ad::TapeScope scope(tape);
        const ad::Real kk = tape.Register(k);
        const ad::Real w = TotalVariance(Passive(p), kk);
        tape.AddAdjoint(w, 1.0);
        tape.SweepBack(tape.GetPosition(), ad::Position{});
        const double allowed = 8.0 * kEps * p.b * (std::fabs(p.rho) + (std::fabs(x) / r));
        EXPECT_LE(std::fabs(tape.Adjoint(kk) - d.w1), allowed) << "w' at k " << k;
      }
      {
        ad::Tape tape;
        const ad::TapeScope scope(tape);
        const ad::Real kk = tape.Register(k);
        const ad::Real w1 = Evaluate(Passive(p), kk).w1;
        tape.AddAdjoint(w1, 1.0);
        tape.SweepBack(tape.GetPosition(), ad::Position{});
        const double allowed = 8.0 * kEps * p.b * ((1.0 / r) + (x * x / (r * r * r)));
        EXPECT_LE(std::fabs(tape.Adjoint(kk) - d.w2), allowed) << "w'' at k " << k;
      }
    }
  }
}

// d^2 C / dK^2 at K, F = 1, T = 1, the vol at each strike from the slice, from out-of-the-money
// prices.
double DensityByDifferences(const Params& p, double strike, double h) {
  const OptionType type = strike < 1.0 ? OptionType::kPut : OptionType::kCall;
  const auto price = [&p, type](double kk) {
    const double vol = std::sqrt(TotalVariance(p, std::log(kk)));
    return Black76Price(1.0, 1.0, kk, 1.0, vol, type);
  };
  return (price(strike + h) - (2.0 * price(strike)) + price(strike - h)) / (h * h);
}

TEST(RawSvi, DurrlemanGIsTheDensityOfTheCallPricesItImplies) {
  constexpr double kTolerance = 1e-6;
  for (const Params& p : kSlices) {
    for (const double k : kPoints) {
      const double strike = std::exp(k);
      const double h = 1e-3 * strike;
      const double coarse = DensityByDifferences(p, strike, h);
      const double fine = DensityByDifferences(p, strike, 0.5 * h);
      const double density = ((4.0 * fine) - coarse) / 3.0;
      const double w = TotalVariance(p, k);
      const double d_minus = DMinus(w, k);
      const double phi = std::exp(-0.5 * d_minus * d_minus) / std::sqrt(2.0 * std::acos(-1.0));
      const double g_from_prices = density * strike * std::sqrt(w) / phi;
      EXPECT_NEAR(g_from_prices, Durrleman(p, k), kTolerance) << "k " << k;
    }
  }
  // The density is negative where Vogt's g is (GJ Figure 1).
  EXPECT_LT(Durrleman(kVogt, 0.879), -0.03);
  EXPECT_LT(DensityByDifferences(kVogt, std::exp(0.879), 1e-3), 0.0);
}

TEST(RawSvi, DPlusFollowsMartiniMingoneLemma21AtLargeK) {
  const auto d_plus = [](const Params& p, double k) { return DPlus(TotalVariance(p, k), k); };
  // b (1 + rho) = 1.8 < 2: d+ goes to -infinity.
  const Params below{.a = 0.02, .b = 1.2, .rho = 0.5, .m = 0.0, .sigma = 0.2};
  EXPECT_LT(d_plus(below, 1e8), d_plus(below, 1e4));
  EXPECT_LT(d_plus(below, 1e8), -100.0);
  // b (1 + rho) = 2.25 > 2, BENCHMARKS.md SVI-4: d+ goes to +infinity.
  const Params above{.a = 0.02, .b = 1.5, .rho = 0.5, .m = 0.0, .sigma = 0.2};
  EXPECT_GT(d_plus(above, 1e8), d_plus(above, 1e4));
  EXPECT_GT(d_plus(above, 1e8), 100.0);
  // b (1 + rho) = 2 exactly: d+ tends to 0.
  const Params at{.a = 0.02, .b = 2.0, .rho = 0.0, .m = 0.0, .sigma = 0.2};
  EXPECT_LT(std::fabs(d_plus(at, 1e8)), 1e-3);
}

TEST(RawSvi, DPlusAndDMinusDifferBySqrtW) {
  for (const Params& p : kSlices) {
    for (const double k : kPoints) {
      const double w = TotalVariance(p, k);
      EXPECT_NEAR(DPlus(w, k) - DMinus(w, k), std::sqrt(w), 4.0 * kEps * std::fabs(k / w));
    }
  }
}

}  // namespace
}  // namespace aadp::svi
