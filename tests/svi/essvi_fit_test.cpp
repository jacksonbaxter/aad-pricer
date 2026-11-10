// The eSSVI surface fit (src/svi/essvi_fit.hpp) on hand-made parameters only, never chain data.
// Every surface, seed and tolerance here was stated in NOTES.md, "Stage 7c", before any of these
// tests ran.
#include "svi/essvi_fit.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "rng/normal.hpp"
#include "rng/philox.hpp"
#include "svi/essvi.hpp"
#include "svi/fit.hpp"
#include "svi/implied_vol.hpp"

namespace aadp::svi {
namespace {

using Slice = EssviSlice<double>;
using Surface = std::vector<Slice>;

constexpr double kForward = 22800.0;
constexpr double kSigmaLeg = 2.0;
// A development seed, never 1729 or 271828.
constexpr std::uint64_t kNoiseSeed = 20261122;

const Surface kH = {{.theta = 4.4e-4, .rho = -0.5, .psi = 0.02},
                    {.theta = 1.0e-3, .rho = -0.45, .psi = 0.03},
                    {.theta = 2.8e-3, .rho = -0.5, .psi = 0.05}};

struct Shape {
  double expiry;
  double discount;
  double first_strike;
  double last_strike;
};

constexpr std::array<Shape, 3> kShapes = {
    {{.expiry = 5.0 / 365.0, .discount = 0.9999, .first_strike = 19700.0, .last_strike = 24100.0},
     {.expiry = 12.0 / 365.0, .discount = 0.9995, .first_strike = 19500.0, .last_strike = 24500.0},
     {.expiry = 33.0 / 365.0, .discount = 0.998, .first_strike = 18000.0, .last_strike = 25000.0}}};

// TAIFEX Rules Art. 7, as src/data/calibration_set.cpp has it; the svi area does not link data.
double Tick(double price) {
  if (price < 10.0) return 0.1;
  if (price < 50.0) return 0.5;
  if (price < 500.0) return 1.0;
  if (price < 1000.0) return 5.0;
  return 10.0;
}

// Out-of-the-money strikes on the 100-point grid, the forward excluded; with noisy, sigma_i z_i
// from Philox on (kNoiseSeed, strike index, slice); then prices of at least 1 point only.
std::vector<FitSlice> Quotes(const Surface& s, bool noisy) {
  std::vector<FitSlice> out;
  for (std::size_t j = 0; j < s.size(); ++j) {
    const Shape& sh = kShapes[j];
    FitSlice slice{.forward = kForward, .discount = sh.discount, .expiry = sh.expiry, .quotes = {}};
    const auto strikes = static_cast<std::uint64_t>((sh.last_strike - sh.first_strike) / 100.0);
    for (std::uint64_t index = 0; index <= strikes; ++index) {
      const double strike = sh.first_strike + (100.0 * static_cast<double>(index));
      if (strike == kForward) continue;
      const OptionType type = strike < kForward ? OptionType::kPut : OptionType::kCall;
      const double w = EssviTotalVariance(s[j], std::log(strike / kForward));
      double price =
          Black76Price(kForward, sh.discount, strike, sh.expiry, std::sqrt(w / sh.expiry), type);
      const double tick = Tick(price);
      const double noise = std::sqrt((kSigmaLeg * kSigmaLeg) + (tick * tick / 12.0));
      if (noisy) {
        price +=
            noise * inverse_normal(PathUniform(kNoiseSeed, index, static_cast<std::uint32_t>(j)));
      }
      if (price < 1.0) continue;
      slice.quotes.push_back({.strike = strike, .price = price, .type = type, .noise = noise});
    }
    out.push_back(std::move(slice));
  }
  return out;
}

double Param(const Surface& s, std::size_t j) {
  const Slice& sl = s[j / 3];
  if (j % 3 == 0) return sl.theta;
  return j % 3 == 1 ? sl.rho : sl.psi;
}

Surface Fitted(const EssviFitResult& r) {
  Surface out;
  for (const EssviSliceFit& s : r.slices) out.push_back(s.params);
  return out;
}

// The rounding that separates Phi_j from stage 6b's Objective at the converted parameters, added to
// the registered 1e-9 relative (NOTES.md, "Stage 7c results", decisions after the
// pre-registration): the two paths to w differ by rounding (64 eps S), which moves each price by
// rounding at its own scale. A rounding scale, not a proof: each residual is allowed to move by
// d_i = 1024 eps V_i / sigma_i, V_i the quoted price, so r_i^2 by d_i (2 |r_i| + d_i). It
// matters only where Phi_j is itself near rounding level, as in an exact fit.
double ObjectiveRounding(const FitSlice& slice, const std::vector<double>& residual) {
  double out = 0.0;
  for (std::size_t i = 0; i < slice.quotes.size() && i < residual.size(); ++i) {
    const FitQuote& q = slice.quotes[i];
    const double d = 1024.0 * 0x1p-52 * q.price / q.noise;
    out += d * ((2.0 * std::fabs(residual[i])) + d);
  }
  return out;
}

// Every slice's Phi is stage 6b's Objective at the converted parameters, and they add up.
void ExpectStage6bObjective(std::span<const FitSlice> quotes, const EssviFitResult& r) {
  double total = 0.0;
  for (std::size_t j = 0; j < r.slices.size(); ++j) {
    const double phi6b = Objective(quotes[j], r.slices[j].raw);
    EXPECT_NEAR(r.slices[j].objective, phi6b,
                (1e-9 * phi6b) + ObjectiveRounding(quotes[j], r.slices[j].residual))
        << "slice " << j;
    total += r.slices[j].objective;
  }
  EXPECT_NEAR(r.objective, total, 1e-12 * total + 1e-300);
}

void ExpectWithinNoiselessTolerance(std::span<const FitSlice> quotes, const Surface& truth,
                                    const EssviFitResult& r) {
  const Surface fit = Fitted(r);
  const std::vector<double> c = EssviParameterCovariance(quotes, fit);
  ASSERT_EQ(c.size(), 81U);
  const EssviGlobalPoint g = ToGlobal(truth);
  ASSERT_TRUE(g.mapped);
  const double phi_true = EssviObjective(quotes, g.x);
  for (std::size_t j = 0; j < 9; ++j) {
    const double cjj = c[(j * 9) + j];
    const double tol = 2.0 * (std::sqrt(r.objective * cjj) + std::sqrt(phi_true * cjj));
    const double err = std::fabs(Param(fit, j) - Param(truth, j));
    EXPECT_LE(err, tol) << "parameter " << j;
    std::printf("  parameter %zu: error %.3g, tolerance %.3g (%.2f of it)\n", j, err, tol,
                tol > 0.0 ? err / tol : 0.0);
  }
}

TEST(EssviFit, RecoversTheHandMadeSurfaceFromExactPrices) {
  const std::vector<FitSlice> quotes = Quotes(kH, false);
  const auto r = FitEssviSurface(quotes);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->converged) << EssviStopName(r->stop);
  EXPECT_LE(r->objective, 1e-10);
  EXPECT_EQ(r->constraints.violations, 0U);
  ExpectWithinNoiselessTolerance(quotes, kH, *r);
  ExpectStage6bObjective(quotes, *r);
}

TEST(EssviFit, RecoversTheHandMadeSurfaceWithinFourStandardErrorsOfNoise) {
  const std::vector<FitSlice> quotes = Quotes(kH, true);
  const auto r = FitEssviSurface(quotes);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->converged) << EssviStopName(r->stop);
  EXPECT_EQ(r->constraints.violations, 0U);
  const Surface fit = Fitted(*r);
  const std::vector<double> c = EssviParameterCovariance(quotes, fit);
  ASSERT_EQ(c.size(), 81U);
  for (std::size_t j = 0; j < 9; ++j) {
    const double se = std::sqrt(c[(j * 9) + j]);
    const double z = (Param(fit, j) - Param(kH, j)) / se;
    EXPECT_LE(std::fabs(z), 4.0) << "parameter " << j;
    std::printf("  parameter %zu: z %.3f\n", j, z);
  }
  std::printf("  Phi %.4f over %zu, %zu and %zu quotes\n", r->objective, quotes[0].quotes.size(),
              quotes[1].quotes.size(), quotes[2].quotes.size());
  ExpectStage6bObjective(quotes, *r);
}

TEST(EssviFit, RecoversASurfaceOnTheB2CapWithTheCapAtItsEnd) {
  Surface hb = kH;
  hb[1].psi = 0.04;
  EssviGlobalPoint g = ToGlobal(hb);
  ASSERT_TRUE(g.mapped);
  ASSERT_TRUE(g.clamps.empty());
  g.x[6] = 1.0;
  std::vector<PsiInterval> iv;
  const Surface truth = FromGlobal<double>(g.x, &iv);
  ASSERT_EQ(iv[0].cap, PsiCap::kGatheralJacquier);
  const std::vector<FitSlice> quotes = Quotes(truth, false);
  const auto r = FitEssviSurface(quotes);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->converged) << EssviStopName(r->stop);
  EXPECT_LE(r->objective, 1e-10);
  EXPECT_TRUE(r->coordinates[6].at_upper) << r->coordinates[6].value;
  EXPECT_EQ(r->slices[0].interval.cap, PsiCap::kGatheralJacquier);
  ExpectWithinNoiselessTolerance(quotes, truth, *r);
}

TEST(EssviFit, DataOutsideSLandsOnSWithEveryConstraintHeld) {
  Surface outside = kH;
  outside[1].psi = 1.05 * kH[0].psi * kH[1].theta / kH[0].theta;
  outside[2].psi = 0.06;
  const EssviConstraintReport before = CheckEssviConstraints(outside);
  ASSERT_EQ(before.violations, 1U);
  const std::vector<FitSlice> quotes = Quotes(outside, false);
  const auto r = FitEssviSurface(quotes);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->converged) << EssviStopName(r->stop);
  EXPECT_TRUE(r->coordinates[7].at_upper) << r->coordinates[7].value;
  EXPECT_EQ(r->slices[1].interval.cap, PsiCap::kThetaRatio);
  EXPECT_EQ(r->constraints.violations, 0U);
  EXPECT_GT(r->objective, 0.0);
  const double again = EssviObjective(quotes, r->x);
  EXPECT_NEAR(r->objective, again, 1e-12 * again);
  std::printf("  Phi %.6g at the S cap\n", r->objective);
  ExpectStage6bObjective(quotes, *r);
}

TEST(EssviFit, ErrorsAreValues) {
  const std::vector<FitSlice> good = Quotes(kH, false);
  EXPECT_EQ(FitEssviSurface(std::span<const FitSlice>{}).error().code,
            EssviFitErrorCode::kSliceCount);
  std::vector<FitSlice> many;
  many.reserve(9);
  for (int i = 0; i < 9; ++i) many.push_back(good[0]);
  EXPECT_EQ(FitEssviSurface(many).error().code, EssviFitErrorCode::kSliceCount);
  std::vector<FitSlice> swapped = {good[1], good[0], good[2]};
  const auto order = FitEssviSurface(swapped);
  EXPECT_EQ(order.error().code, EssviFitErrorCode::kExpiryOrder);
  EXPECT_EQ(order.error().slice, 1U);
  std::vector<FitSlice> bad = good;
  bad[2].forward = std::numeric_limits<double>::quiet_NaN();
  const auto slice = FitEssviSurface(bad);
  EXPECT_EQ(slice.error().code, EssviFitErrorCode::kSlice);
  EXPECT_EQ(slice.error().slice, 2U);
  EXPECT_EQ(slice.error().fit.code, FitErrorCode::kBadSlice);
  bad = good;
  bad[1].quotes[3].noise = 0.0;
  const auto quote = FitEssviSurface(bad);
  EXPECT_EQ(quote.error().fit.code, FitErrorCode::kBadQuote);
  EXPECT_EQ(quote.error().fit.quote, 3U);
  EXPECT_EQ(quote.error().slice, 1U);
  bad = good;
  bad[0].quotes.resize(4);
  EXPECT_EQ(FitEssviSurface(bad).error().fit.code, FitErrorCode::kTooFewStrikes);
  bad = good;
  bad[0].quotes[0].price = 1e9;  // above the discounted forward: no implied vol
  EXPECT_EQ(FitEssviSurface(bad).error().fit.code, FitErrorCode::kNoImpliedVol);
  for (const auto code : {EssviFitErrorCode::kSliceCount, EssviFitErrorCode::kExpiryOrder,
                          EssviFitErrorCode::kSlice, EssviFitErrorCode::kNoFeasibleStart}) {
    EXPECT_STRNE(EssviFitErrorName(code), "unknown");
  }
}

}  // namespace
}  // namespace aadp::svi
