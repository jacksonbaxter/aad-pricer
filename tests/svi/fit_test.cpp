// The raw SVI fit (src/svi/fit.hpp) on hand-made parameters only, never chain data. Every slice,
// seed and tolerance here was stated in docs/NOTES.md, "Stage 6b", before any of these tests ran.
#include "svi/fit.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "rng/normal.hpp"
#include "rng/philox.hpp"
#include "svi/arbitrage.hpp"
#include "svi/implied_vol.hpp"
#include "svi/raw_svi.hpp"

namespace aadp::svi {
namespace {

constexpr double kForward = 22800.0;
constexpr double kSigmaLeg = 2.0;
// A development seed, never 1729 or 271828.
constexpr std::uint64_t kNoiseSeed = 20261105;

// TAIFEX Rules Art. 7, as src/data/calibration_set.cpp has it; the svi area does not link data.
double Tick(double price) {
  if (price < 10.0) return 0.1;
  if (price < 50.0) return 0.5;
  if (price < 500.0) return 1.0;
  if (price < 1000.0) return 5.0;
  return 10.0;
}

struct Spec {
  const char* name;
  double expiry;
  double discount;
  SviParams params;
  double first_strike;
  double last_strike;
};

const Spec kA{.name = "A",
              .expiry = 5.0 / 365.0,
              .discount = 0.9999,
              .params = {.a = 1e-4, .b = 0.012, .rho = -0.45, .m = 0.002, .sigma = 0.025},
              .first_strike = 19700.0,
              .last_strike = 24100.0};
const Spec kB{.name = "B",
              .expiry = 12.0 / 365.0,
              .discount = 0.9995,
              .params = {.a = 2e-4, .b = 0.02, .rho = -0.35, .m = 0.005, .sigma = 0.04},
              .first_strike = 19500.0,
              .last_strike = 24500.0};
const Spec kC{.name = "C",
              .expiry = 33.0 / 365.0,
              .discount = 0.998,
              .params = {.a = 5e-4, .b = 0.04, .rho = -0.5, .m = 0.01, .sigma = 0.06},
              .first_strike = 18000.0,
              .last_strike = 25000.0};

OptionType OtmType(double strike, double forward) {
  return strike < forward ? OptionType::kPut : OptionType::kCall;
}

double Price(const Spec& s, double strike) {
  const double w = TotalVariance(s.params, std::log(strike / kForward));
  return Black76Price(kForward, s.discount, strike, s.expiry, std::sqrt(w / s.expiry),
                      OtmType(strike, kForward));
}

// Out-of-the-money strikes on the 100-point grid, the forward itself excluded. noise_slice >= 0
// adds sigma_i z_i, z_i from Philox on (kNoiseSeed, strike index, noise_slice). Then only prices
// of at least 1 point are kept, as the calibration filter keeps observed settlements.
FitSlice MakeSlice(const Spec& s, const std::vector<double>& strikes, int noise_slice = -1) {
  FitSlice slice{.forward = kForward, .discount = s.discount, .expiry = s.expiry, .quotes = {}};
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double strike = strikes[i];
    double price = Price(s, strike);
    const double noise = std::sqrt((kSigmaLeg * kSigmaLeg) + (Tick(price) * Tick(price) / 12.0));
    if (noise_slice >= 0) {
      const double u = PathUniform(kNoiseSeed, i, static_cast<std::uint32_t>(noise_slice));
      price += noise * inverse_normal(u);
    }
    if (!(price >= 1.0)) continue;
    const double final_noise =
        std::sqrt((kSigmaLeg * kSigmaLeg) + (Tick(price) * Tick(price) / 12.0));
    slice.quotes.push_back({.strike = strike,
                            .price = price,
                            .type = OtmType(strike, kForward),
                            .noise = final_noise});
  }
  return slice;
}

std::vector<double> GridStrikes(const Spec& s) {
  std::vector<double> out;
  for (int i = 0;; ++i) {
    const double strike = s.first_strike + (100.0 * i);
    if (strike > s.last_strike) break;
    if (strike != kForward) out.push_back(strike);
  }
  return out;
}

std::array<double, 5> AsArray(const SviParams& p) { return {p.a, p.b, p.rho, p.m, p.sigma}; }

constexpr std::array<const char*, 5> kNames = {"a", "b", "rho", "m", "sigma"};

std::string Padded(std::size_t j) {
  std::string out = kNames[j];
  out.resize(5, ' ');
  return out;
}

std::string Label(const std::string& name, std::size_t quotes, std::size_t j) {
  return name + " n=" + std::to_string(quotes) + " " + Padded(j);
}

// The noiseless tolerance: |theta_hat_j - theta_j| <= 2 (sqrt(Phi_hat C_jj) + sqrt(Phi_true
// C_jj)), with Phi_hat <= 1e-10 and convergence. Prints each error beside its tolerance.
void ExpectNoiselessRecovery(const char* name, const FitSlice& slice, const SviParams& truth,
                             const FitResult& fit) {
  EXPECT_TRUE(fit.converged) << name;
  EXPECT_LE(fit.objective, 1e-10) << name;
  const double phi_true = Objective(slice, truth);
  const auto cov = ParameterCovariance(slice, fit.params);
  ASSERT_TRUE(cov.has_value()) << name;
  const Matrix5 covariance = cov.value_or(Matrix5{});
  const auto got = AsArray(fit.params);
  const auto want = AsArray(truth);
  for (std::size_t j = 0; j < 5; ++j) {
    const double c = covariance[j][j];
    const double tolerance = 2.0 * (std::sqrt(fit.objective * c) + std::sqrt(phi_true * c));
    const double error = std::abs(got[j] - want[j]);
    // A std::string label, because g++ 14 warns that a %s argument here might be null.
    const std::string label = Label(name, fit.quotes, j);
    std::printf("%s fitted %.17g true %.17g error %.3g tolerance %.3g (Phi %.3g, Phi_true %.3g)\n",
                label.c_str(), got[j], want[j], error, tolerance, fit.objective, phi_true);
    EXPECT_LE(error, tolerance) << name << " " << kNames[j];
  }
}

TEST(FitSynthetic, NoiselessPricesRecoverTheParameters) {
  for (const Spec* s : {&kA, &kB, &kC}) {
    const FitSlice slice = MakeSlice(*s, GridStrikes(*s));
    const auto fit = FitRawSvi(slice);
    ASSERT_TRUE(fit.has_value()) << s->name << " " << FitErrorName(fit.error().code);
    ExpectNoiselessRecovery(s->name, slice, s->params, *fit);
    EXPECT_TRUE(fit->ActiveBounds().empty()) << s->name;
  }
}

// Within 4 standard errors, SE_j = sqrt(C_jj) at the fit, for every parameter of A, B and C.
TEST(FitSynthetic, NoisyPricesRecoverTheParametersWithinFourStandardErrors) {
  int slice_index = 0;
  for (const Spec* s : {&kA, &kB, &kC}) {
    const FitSlice slice = MakeSlice(*s, GridStrikes(*s), slice_index++);
    const auto fit = FitRawSvi(slice);
    ASSERT_TRUE(fit.has_value()) << s->name << " " << FitErrorName(fit.error().code);
    EXPECT_TRUE(fit->converged) << s->name;
    const auto cov = ParameterCovariance(slice, fit->params);
    ASSERT_TRUE(cov.has_value()) << s->name;
    const Matrix5 covariance = cov.value_or(Matrix5{});
    const auto got = AsArray(fit->params);
    const auto want = AsArray(s->params);
    for (std::size_t j = 0; j < 5; ++j) {
      const double se = std::sqrt(covariance[j][j]);
      const double z = (got[j] - want[j]) / se;
      const std::string label = std::string(s->name) + " n=" + std::to_string(fit->quotes);
      std::printf("%s Phi=%.4g %s fitted %.10g true %.10g se %.3g z %+.3f\n", label.c_str(),
                  fit->objective, Padded(j).c_str(), got[j], want[j], se, z);
      EXPECT_LE(std::abs(z), 4.0) << s->name << " " << kNames[j];
    }
  }
}

TEST(FitSynthetic, RhoAtMinusOneIsRecoveredWithinTheNoiselessTolerance) {
  Spec s = kA;
  s.params.rho = -1.0;
  const FitSlice slice = MakeSlice(s, GridStrikes(s));
  const auto fit = FitRawSvi(slice);
  ASSERT_TRUE(fit.has_value()) << FitErrorName(fit.error().code);
  const auto cov = ParameterCovariance(slice, fit->params);
  ASSERT_TRUE(cov.has_value());
  const Matrix5 covariance = cov.value_or(Matrix5{});
  const double c = covariance[2][2];
  const double tolerance =
      2.0 * (std::sqrt(fit->objective * c) + std::sqrt(Objective(slice, s.params) * c));
  std::printf("rho=-1: fitted rho %.17g, tolerance %.3g, slack %.3g, active %d\n", fit->params.rho,
              tolerance, fit->bounds[0].slack, static_cast<int>(fit->bounds[0].active));
  EXPECT_LE(std::abs(fit->params.rho + 1.0), tolerance);
  EXPECT_LE(fit->objective, 1e-10);
}

// Data from rho = -1.05, outside the domain but with positive variance on its strikes: the
// constrained optimum sits on rho = -1, which must be exact and recorded.
TEST(FitSynthetic, RhoBeyondItsBoundFitsTheBoundAndRecordsIt) {
  Spec lower = kA;
  lower.params.rho = -1.05;
  const FitSlice low_slice = MakeSlice(lower, GridStrikes(lower));
  const auto low_fit = FitRawSvi(low_slice);
  ASSERT_TRUE(low_fit.has_value()) << FitErrorName(low_fit.error().code);
  EXPECT_EQ(low_fit->params.rho, -1.0);
  // The reported parameters give the reported Phi: the bound held in the solve, not only in the
  // conversion to raw parameters, which clamps rho.
  EXPECT_NEAR(Objective(low_slice, low_fit->params), low_fit->objective,
              1e-12 + (1e-9 * low_fit->objective));
  EXPECT_TRUE(low_fit->bounds[static_cast<std::size_t>(Bound::kRhoLower)].active);
  EXPECT_FALSE(low_fit->bounds[static_cast<std::size_t>(Bound::kRhoUpper)].active);
  const auto low_active = low_fit->ActiveBounds();
  ASSERT_FALSE(low_active.empty());
  EXPECT_EQ(low_active.front(), Bound::kRhoLower);

  // The mirror: k -> -k, so rho = +1.05, m = -0.002, and strikes F^2 / K.
  Spec upper = kA;
  upper.params.rho = 1.05;
  upper.params.m = -0.002;
  std::vector<double> reflected;
  for (const double strike : GridStrikes(kA)) reflected.push_back(kForward * kForward / strike);
  const FitSlice high_slice = MakeSlice(upper, reflected);
  const auto high_fit = FitRawSvi(high_slice);
  ASSERT_TRUE(high_fit.has_value()) << FitErrorName(high_fit.error().code);
  EXPECT_EQ(high_fit->params.rho, 1.0);
  EXPECT_NEAR(Objective(high_slice, high_fit->params), high_fit->objective,
              1e-12 + (1e-9 * high_fit->objective));
  EXPECT_TRUE(high_fit->bounds[static_cast<std::size_t>(Bound::kRhoUpper)].active);
  EXPECT_FALSE(high_fit->bounds[static_cast<std::size_t>(Bound::kRhoLower)].active);
  std::printf("rho beyond: lower fit rho %.17g Phi %.4g; upper fit rho %.17g Phi %.4g\n",
              low_fit->params.rho, low_fit->objective, high_fit->params.rho, high_fit->objective);
}

// T = 1, F = 100, D = 1, k from -1.5 to 1.5 in steps of 0.05, out of the money, noise 0.01.
FitSlice UnitSlice(const SviParams& p) {
  FitSlice slice{.forward = 100.0, .discount = 1.0, .expiry = 1.0, .quotes = {}};
  for (int i = -30; i <= 30; ++i) {
    if (i == 0) continue;
    const double k = 0.05 * i;
    const double strike = 100.0 * std::exp(k);
    const OptionType type = i < 0 ? OptionType::kPut : OptionType::kCall;
    const double w = TotalVariance(p, k);
    slice.quotes.push_back({.strike = strike,
                            .price = Black76Price(100.0, 1.0, strike, 1.0, std::sqrt(w), type),
                            .type = type,
                            .noise = 0.01});
  }
  return slice;
}

bool HasKind(const std::vector<Violation>& v, ViolationKind kind) {
  return std::ranges::any_of(v, [kind](const Violation& x) { return x.kind == kind; });
}

TEST(FitNeverRepairs, VogtIsFittedAsItIsAndItsDensityViolationReported) {
  const SviParams vogt{.a = -0.0410, .b = 0.1331, .rho = 0.3060, .m = 0.3586, .sigma = 0.4153};
  const FitSlice slice = UnitSlice(vogt);
  const auto fit = FitRawSvi(slice);
  ASSERT_TRUE(fit.has_value()) << FitErrorName(fit.error().code);
  ExpectNoiselessRecovery("vogt", slice, vogt, *fit);
  const ButterflyReport report = CheckButterfly(fit->params);
  EXPECT_TRUE(HasKind(report.violations, ViolationKind::kDensity));
  EXPECT_LT(report.min_g, 0.0);
}

TEST(FitNeverRepairs, SlopeAboveTwoIsFittedAndItsWingViolationsReported) {
  const SviParams steep{.a = 0.02, .b = 1.5, .rho = 0.5, .m = 0.0, .sigma = 0.2};
  const FitSlice slice = UnitSlice(steep);
  const auto fit = FitRawSvi(slice);
  ASSERT_TRUE(fit.has_value()) << FitErrorName(fit.error().code);
  ExpectNoiselessRecovery("slope-2.25", slice, steep, *fit);
  const ButterflyReport report = CheckButterfly(fit->params);
  EXPECT_TRUE(HasKind(report.violations, ViolationKind::kRightWing));
  EXPECT_TRUE(HasKind(report.violations, ViolationKind::kDPlusLimit));
}

TEST(FitNeverRepairs, ACrossingPairFittedSliceBySliceIsReportedByTheCalendarCheck) {
  Spec later = kB;
  later.expiry = 33.0 / 365.0;
  later.params.a -= 1e-4;
  const auto earlier_fit = FitRawSvi(MakeSlice(kB, GridStrikes(kB)));
  const auto later_fit = FitRawSvi(MakeSlice(later, GridStrikes(later)));
  ASSERT_TRUE(earlier_fit.has_value());
  ASSERT_TRUE(later_fit.has_value());
  const CalendarReport report = CheckCalendar(earlier_fit->params, later_fit->params);
  EXPECT_TRUE(HasKind(report.violations, ViolationKind::kCalendarCrossing));
  EXPECT_LT(report.min_gap, 0.0);
}

TEST(FitMetrics, MeanAbsolutePriceErrorInBasisPointsOfTheForward) {
  FitSlice slice{.forward = 20000.0, .discount = 1.0, .expiry = 0.1, .quotes = {}};
  slice.quotes = {{.strike = 19000, .price = 10.0, .type = OptionType::kPut, .noise = 1.0},
                  {.strike = 21000, .price = 12.0, .type = OptionType::kCall, .noise = 1.0}};
  const std::vector<double> model = {12.0, 8.0};
  // (2 + 4) / 2 = 3 points on 20000 is 1.5 bp.
  EXPECT_DOUBLE_EQ(MeanAbsPriceErrorBp(slice, model), 1.5);
  EXPECT_TRUE(std::isnan(MeanAbsPriceErrorBp(slice, std::vector<double>{1.0})));
}

TEST(FitMetrics, ImpliedVolRmseInVolPointsUnweighted) {
  const std::vector<double> model = {0.20, 0.25, 0.31};
  const std::vector<double> market = {0.21, 0.23, 0.31};
  // Errors -0.01, 0.02, 0: mean square 5e-4 / 3, RMSE 0.012910 = 1.2910 vol points.
  EXPECT_NEAR(ImpliedVolRmse(model, market), 100.0 * std::sqrt(5e-4 / 3.0), 1e-12);
  EXPECT_TRUE(std::isnan(ImpliedVolRmse(model, std::vector<double>{0.2})));
}

// Errors of 2e154 square to 4e308, past the largest double; the RMSE, 2e154 = 2e156 vol points,
// is finite.
TEST(FitMetrics, ImpliedVolRmseWhereTheSquaredErrorsOverflow) {
  const std::vector<double> model = {3e154, 3e154, 3e154, 3e154};
  const std::vector<double> market = {1e154, 1e154, 1e154, 1e154};
  EXPECT_DOUBLE_EQ(ImpliedVolRmse(model, market), 100.0 * (3e154 - 1e154));
  // Errors 3e154 and -4e154: mean square 12.5e308.
  const std::vector<double> mixed_model = {4e154, 1e154};
  const std::vector<double> mixed_market = {1e154, 5e154};
  EXPECT_NEAR(ImpliedVolRmse(mixed_model, mixed_market) / (100.0 * std::sqrt(12.5) * 1e154), 1.0,
              1e-14);
  // A vol that is not finite still gives what it did: inf, or NaN.
  constexpr double kInf = std::numeric_limits<double>::infinity();
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  EXPECT_TRUE(std::isinf(ImpliedVolRmse(std::vector<double>{kInf}, std::vector<double>{0.2})));
  EXPECT_TRUE(std::isnan(
      ImpliedVolRmse(std::vector<double>{kNaN, 3e154}, std::vector<double>{0.2, 1e154})));
}

// The fuzzer's case, hand-made: at T = 1e-308 every vol, sqrt(w / T), is near 1e154 while w is
// O(1). Total variances alternating 0.01 and 1.5 on 10 strikes leave the flat-ish fit about
// 6e153 from every market vol, so the squared errors sum past the largest double. The reference
// takes the RMSE in sqrt(w) units, where nothing overflows, and divides by sqrt(T).
TEST(FitMetrics, ImpliedVolRmseFiniteAtATinyExpiry) {
  constexpr double kExpiry = 1e-308;
  constexpr std::size_t kStrikes = 10;
  FitSlice slice{.forward = 100.0, .discount = 1.0, .expiry = kExpiry, .quotes = {}};
  for (std::size_t i = 0; i < kStrikes; ++i) {
    const double strike = 100.0 * std::exp(0.1 * (static_cast<double>(i) - 4.5));
    const double w = i % 2 == 0 ? 0.01 : 1.5;
    const OptionType type = OtmType(strike, 100.0);
    slice.quotes.push_back(
        {.strike = strike,
         .price = Black76Price(100.0, 1.0, strike, kExpiry, std::sqrt(w / kExpiry), type),
         .type = type,
         .noise = 1.0});
  }
  const auto fit = FitRawSvi(slice);
  ASSERT_TRUE(fit.has_value());
  const double root_t = std::sqrt(kExpiry);
  double sum = 0.0;
  for (std::size_t i = 0; i < kStrikes; ++i) {
    ASSERT_TRUE(std::isfinite(fit->model_vol[i]) && std::isfinite(fit->market_vol[i]));
    const double e = (fit->model_vol[i] - fit->market_vol[i]) * root_t;
    sum += e * e;
  }
  const double reference = 100.0 * std::sqrt(sum / static_cast<double>(kStrikes)) / root_t;
  ASSERT_TRUE(std::isfinite(reference));
  const double rmse = ImpliedVolRmse(fit->model_vol, fit->market_vol);
  ASSERT_TRUE(std::isfinite(rmse)) << rmse;
  EXPECT_NEAR(rmse / reference, 1.0, 1e-12);
}

TEST(FitErrors, AreValuesWithNames) {
  const FitSlice good = MakeSlice(kA, GridStrikes(kA));
  FitSlice bad = good;
  bad.forward = -1.0;
  EXPECT_EQ(FitRawSvi(bad).error().code, FitErrorCode::kBadSlice);
  bad = good;
  bad.expiry = std::nan("");
  EXPECT_EQ(FitRawSvi(bad).error().code, FitErrorCode::kBadSlice);
  bad = good;
  bad.quotes[3].noise = 0.0;
  EXPECT_EQ(FitRawSvi(bad).error().code, FitErrorCode::kBadQuote);
  EXPECT_EQ(FitRawSvi(bad).error().quote, 3U);
  bad = good;
  bad.quotes.resize(4);
  EXPECT_EQ(FitRawSvi(bad).error().code, FitErrorCode::kTooFewStrikes);
  bad = good;
  for (FitQuote& q : bad.quotes) q.strike = bad.quotes.front().strike;
  EXPECT_EQ(FitRawSvi(bad).error().code, FitErrorCode::kTooFewStrikes);
  bad = good;
  // A put above its maximum, the discounted strike, has no implied vol.
  bad.quotes[0].price = bad.quotes[0].strike * 2.0;
  const auto no_vol = FitRawSvi(bad);
  ASSERT_FALSE(no_vol.has_value());
  EXPECT_EQ(no_vol.error().code, FitErrorCode::kNoImpliedVol);
  EXPECT_EQ(no_vol.error().quote, 0U);
  EXPECT_EQ(no_vol.error().status, InversionStatus::kAboveMaximum);
  for (const auto code :
       {FitErrorCode::kBadSlice, FitErrorCode::kBadQuote, FitErrorCode::kTooFewStrikes,
        FitErrorCode::kNoImpliedVol, FitErrorCode::kNoFeasibleStart}) {
    EXPECT_STRNE(FitErrorName(code), "unknown");
  }
  for (std::size_t i = 0; i < kBoundCount; ++i) {
    EXPECT_STRNE(BoundName(static_cast<Bound>(i)), "unknown");
  }
}

TEST(FitResultFields, AreConsistent) {
  const FitSlice slice = MakeSlice(kB, GridStrikes(kB));
  const auto fit = FitRawSvi(slice);
  ASSERT_TRUE(fit.has_value());
  EXPECT_EQ(fit->quotes, slice.quotes.size());
  EXPECT_EQ(fit->model_price.size(), slice.quotes.size());
  EXPECT_EQ(fit->market_vol.size(), slice.quotes.size());
  double sum = 0.0;
  for (const double r : fit->residual) sum += r * r;
  EXPECT_DOUBLE_EQ(sum, fit->objective);
  EXPECT_NEAR(Objective(slice, fit->params), fit->objective, 1e-12 + (1e-9 * fit->objective));
  EXPECT_EQ(fit->starts, 5U);
  EXPECT_GE(fit->starts_at_best, 1U);
  for (std::size_t i = 0; i < kBoundCount; ++i) {
    EXPECT_EQ(fit->bounds[i].bound, static_cast<Bound>(i));
  }
}

}  // namespace
}  // namespace aadp::svi
