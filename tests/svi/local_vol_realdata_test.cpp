// The local-volatility pricer on the surface fitted to the pinned captures. Asserts only what
// NOTES.md, "Stage 7a", fixed before the first price: the surface builds from the fit,
// S0 = D_1 F_1, the three contracts validate, a maturity one second past T_3 is refused, prices
// and tallies are finite and consistent, and tape against Dual holds on 64 paths per contract.
// Prices, Greeks and floors are reported by aadp_local_vol_report, not asserted here. The files
// are never committed. With no file the tests skip, visibly, and say how to get it.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ad/bounds.hpp"
#include "ad/local_vol_gradient.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
#include "pricer/draws.hpp"
#include "pricer/dual_ops.hpp"  // IWYU pragma: keep (tests/pricer/: the Dual overloads, by ADL)
#include "pricer/local_vol.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/runner.hpp"
#include "pricer/substep_kernels.hpp"
#include "support/dual.hpp"
#include "svi/chain_fit.hpp"
#include "svi/chain_surface.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::svi {
namespace {

namespace fs = std::filesystem;

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261118;
constexpr std::uint64_t kCheckedPaths = 64;
// The second, empirical check of tests/ad/local_vol_gradient_test.cpp, because AgreementBound is
// vacuous here (NOTES.md, "Stage 7a", decisions after the pre-registration).
constexpr double kScaledAgreement = 0x1p-26;

// Tests run with the repo root as working directory.
fs::path data_dir() {
  const char* override_dir = std::getenv("AADP_DATA_DIR");
  return override_dir != nullptr ? fs::path(override_dir) : fs::path("data");
}

std::optional<std::string> load(std::string_view name) {
  const fs::path path = data_dir() / name;
  if (!fs::exists(path)) {
    return std::nullopt;
  }
  auto bytes = taifex::read_binary_file(path);
  EXPECT_TRUE(bytes.has_value()) << bytes.error();
  return bytes ? std::optional<std::string>(std::move(*bytes)) : std::nullopt;
}

// A macro because GTEST_SKIP has to return from the test body itself.
// NOLINTBEGIN(cppcoreguidelines-macro-usage,bugprone-macro-parentheses): declares a named variable.
#define AADP_LOAD_OR_SKIP(variable, name)                                            \
  const std::optional<std::string> variable = load(name);                            \
  if (!variable) {                                                                   \
    GTEST_SKIP() << "SKIPPED: " << (data_dir() / (name)).string()                    \
                 << " is absent, so this real-file test did not run. Fetch it with " \
                    "scripts/fetch_data.sh.";                                        \
  }
// NOLINTEND(cppcoreguidelines-macro-usage,bugprone-macro-parentheses)

constexpr std::string_view kOptionsFile = "DailyMarketReportOpt.csv";
constexpr std::string_view kFuturesFile = "DailyMarketReportFut.csv";

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

// The chain, fitted as stage 6b fits it, as a surface; empty with a recorded failure otherwise.
std::optional<ChainSurface> Fitted(const std::string& option_bytes,
                                   const std::string& future_bytes) {
  const auto options = taifex::read_options(option_bytes);
  const auto futures = taifex::read_futures(future_bytes);
  EXPECT_TRUE(options.has_value() && futures.has_value());
  if (!options || !futures) return std::nullopt;
  const auto slices = BuildChainSlices(options->rows, futures->rows, kCalibrationExpiries);
  EXPECT_TRUE(slices.has_value());
  if (!slices) return std::nullopt;
  auto surface = SurfaceFromChain(*slices, FitChain(*slices));
  EXPECT_TRUE(surface.has_value());
  return surface;
}

TEST(RealLocalVol, TheSurfaceBuildsWithTheSpotAtTheFirstParityForward) {
  AADP_LOAD_OR_SKIP(option_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(future_bytes, kFuturesFile);
  const std::optional<ChainSurface> fitted = Fitted(*option_bytes, *future_bytes);
  if (!fitted) GTEST_FAIL() << "the chain did not fit";
  const ChainSurface& s = *fitted;
  ASSERT_EQ(s.terms.size(), 3U);
  EXPECT_EQ(ValidateSurface(s.terms, s.params), SurfaceError::kNone);
  const SviSurface<double> surface(s.terms, s.params);
  EXPECT_EQ(Bits(surface.Curve().Spot()), Bits(s.terms[0].discount * s.terms[0].forward));
  EXPECT_EQ(Bits(surface.Curve().LastExpiry()), Bits(s.terms[2].expiry));
}

TEST(RealLocalVol, TheContractsValidateAndAMaturityPastTheLastSliceIsRefused) {
  AADP_LOAD_OR_SKIP(option_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(future_bytes, kFuturesFile);
  const std::optional<ChainSurface> fitted = Fitted(*option_bytes, *future_bytes);
  if (!fitted) GTEST_FAIL() << "the chain did not fit";
  const ChainSurface& s = *fitted;
  const double last = s.terms.back().expiry;
  for (const NamedContract& c : SurfaceContracts(last)) {
    EXPECT_EQ(pricer::ValidateLocalVol(c.contract, s.terms, s.params, {}).code,
              pricer::LocalVolError::kNone)
        << c.name;
    pricer::LocalVolContract late = c.contract;
    late.maturity = last + (1.0 / (365.0 * 86400.0));
    EXPECT_EQ(pricer::ValidateLocalVol(late, s.terms, s.params, {}).code,
              pricer::LocalVolError::kMaturity)
        << c.name;
  }
}

TEST(RealLocalVol, PricesAndTalliesAreFiniteAndConsistent) {
  AADP_LOAD_OR_SKIP(option_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(future_bytes, kFuturesFile);
  const std::optional<ChainSurface> fitted = Fitted(*option_bytes, *future_bytes);
  if (!fitted) GTEST_FAIL() << "the chain did not fit";
  const ChainSurface& s = *fitted;
  const pricer::RunConfig run{.seed = kSeed, .paths = 4096, .batches = 8, .threads = 4};
  for (const NamedContract& c : SurfaceContracts(s.terms.back().expiry)) {
    const auto price = pricer::PriceLocalVol(c.contract, s.terms, s.params, run);
    const auto grad = ad::PriceLocalVolWithGradient(c.contract, s.terms, s.params, run);
    ASSERT_TRUE(price.has_value() && grad.has_value()) << c.name;
    EXPECT_TRUE(std::isfinite(price->value.mean) && price->value.mean >= 0.0) << c.name;
    EXPECT_TRUE(std::isfinite(price->value.standard_error)) << c.name;
    EXPECT_EQ(Bits(grad->value.mean), Bits(price->value.mean)) << c.name;
    ASSERT_EQ(grad->gradient.size(), 15U) << c.name;
    for (const pricer::Estimate& g : grad->gradient) {
      EXPECT_TRUE(std::isfinite(g.mean) && std::isfinite(g.standard_error)) << c.name;
    }
    const pricer::FloorTally& t = price->floors;
    std::uint64_t classes = 0;
    for (const std::uint64_t n : t.by_class) classes += n;
    const std::uint64_t steps =
        std::uint64_t{c.contract.contract.steps} * c.contract.substeps * run.paths;
    EXPECT_EQ(t.paths, run.paths) << c.name;
    EXPECT_EQ(t.path_steps, steps) << c.name;
    EXPECT_EQ(classes, steps) << c.name;
    EXPECT_EQ(t.Floored() + t.Capped() + t.by_class[0], steps) << c.name;
    EXPECT_LE(t.paths_with_floor, t.paths) << c.name;
    EXPECT_LE(t.paths_with_cap, t.paths) << c.name;
    EXPECT_EQ(grad->floors.by_class, t.by_class) << c.name;
    EXPECT_EQ(grad->floors.paths_with_floor, t.paths_with_floor) << c.name;
  }
}

struct Agreement {
  double bound = 0.0;   // max |tape - dual| / AgreementBound
  double scaled = 0.0;  // max |tape - dual| / the path's largest |derivative|
  double tightest = std::numeric_limits<double>::infinity();  // min AgreementBound / that scale
};

// The surface on the Dual, slice `seeded`'s five parameters on the five tangents.
pricer::LocalVolModel<test::Dual> DualModel(const ChainSurface& s, std::size_t seeded,
                                            const pricer::LocalVolContract& lv) {
  std::vector<RawSvi<test::Dual>> params;
  for (std::size_t j = 0; j < s.params.size(); ++j) {
    const RawSvi<double>& p = s.params[j];
    const auto x = [&](double v, std::size_t slot) {
      return j == seeded ? test::Dual::Variable(v, slot) : test::Dual(v);
    };
    params.push_back({.a = x(p.a, 0),
                      .b = x(p.b, 1),
                      .rho = x(p.rho, 2),
                      .m = x(p.m, 3),
                      .sigma = x(p.sigma, 4)});
  }
  return {SviSurface<test::Dual>(s.terms, params), lv.maturity, lv.contract.steps * lv.substeps,
          LocalVolRule{}};
}

// One path's 15 tape derivatives against the three seeded Duals, by both checks.
void ComparePath(const ad::LocalVolPathGradient::PathResult& result,
                 const std::vector<test::Dual>& seeded, const std::string& label,
                 Agreement& worst) {
  double scale = 0.0;
  for (const test::Dual& dual : seeded) {
    for (std::size_t i = 0; i < 5; ++i) scale = std::max(scale, std::fabs(dual.tangent[i]));
  }
  for (std::size_t j = 0; j < seeded.size(); ++j) {
    for (std::size_t i = 0; i < 5; ++i) {
      const std::size_t input = (5 * j) + i;
      const double bound = ad::bounds::AgreementBound(
          result.absolute[input], result.operand_sums[input], result.statements, result.operands);
      const double diff = std::fabs(result.gradient[input] - seeded[j].tangent[i]);
      ASSERT_TRUE(std::isfinite(result.gradient[input])) << label;
      ASSERT_LE(diff, bound) << label << ", parameter " << input;
      ASSERT_LE(diff, kScaledAgreement * scale)
          << label << ", parameter " << input << ": tape " << result.gradient[input] << ", dual "
          << seeded[j].tangent[i];
      if (diff > 0.0) {
        worst.bound = std::max(worst.bound, diff / bound);
        worst.scaled = std::max(worst.scaled, diff / scale);
        worst.tightest = std::min(worst.tightest, bound / scale);
      }
    }
  }
}

TEST(RealLocalVol, TapeAgreesWithTheDualOnSixtyFourPathsPerContract) {
  AADP_LOAD_OR_SKIP(option_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(future_bytes, kFuturesFile);
  const std::optional<ChainSurface> fitted = Fitted(*option_bytes, *future_bytes);
  if (!fitted) GTEST_FAIL() << "the chain did not fit";
  const ChainSurface& s = *fitted;
  for (const NamedContract& c : SurfaceContracts(s.terms.back().expiry)) {
    const pricer::LocalVolContract& lv = c.contract;
    const std::uint32_t steps = lv.contract.steps * lv.substeps;
    const pricer::LocalVolModel<double> model(SviSurface<double>(s.terms, s.params), lv.maturity,
                                              steps, {});
    std::vector<pricer::LocalVolModel<test::Dual>> duals;
    duals.reserve(3);
    for (std::size_t j = 0; j < 3; ++j) duals.push_back(DualModel(s, j, lv));
    ad::LocalVolPathGradient tape(lv, s.terms, s.params, {});
    Agreement worst;
    std::vector<double> normal(steps);
    std::vector<double> crossing(steps);
    const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
    for (std::uint64_t path = 0; path < kCheckedPaths; ++path) {
      pricer::FillPath(kSeed, path, normal, crossing);
      const double value =
          pricer::EvaluateSubstepped<double>(lv.contract, lv.substeps, model, u).value;
      const ad::LocalVolPathGradient::PathResult result = tape.Evaluate(u);
      const std::string label = std::string(c.name) + ", path " + std::to_string(path);
      ASSERT_EQ(Bits(result.value), Bits(value)) << label;
      std::vector<test::Dual> seeded;
      seeded.reserve(duals.size());
      for (const pricer::LocalVolModel<test::Dual>& d : duals) {
        seeded.push_back(
            pricer::EvaluateSubstepped<test::Dual>(lv.contract, lv.substeps, d, u).value);
        ASSERT_EQ(Bits(seeded.back().value), Bits(value)) << label;
      }
      ComparePath(result, seeded, label, worst);
      if (HasFatalFailure()) return;
    }
    std::printf(
        "measured, %s, %llu paths: max |tape - dual| / bound %.3g, / path scale %.3g "
        "(bound 2^-26); smallest bound / path scale %.3g\n",
        c.name, static_cast<unsigned long long>(kCheckedPaths), worst.bound, worst.scaled,
        worst.tightest);
  }
}

}  // namespace
}  // namespace aadp::svi
