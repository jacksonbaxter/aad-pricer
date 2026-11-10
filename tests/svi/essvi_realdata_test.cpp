// The eSSVI surface fit on the pinned captures. Asserts only what NOTES.md, "Stage 7c", fixed
// before the first eSSVI fit on the real chain: the fit returns for all three slices and
// converges, the constraint checker finds no violation, each converted slice passes
// CheckParameters and the converted surface passes ValidateSurface, each Phi_j is stage 6b's
// Objective at the converted parameters within 1e-9, the checks run, and the three contracts
// validate on the eSSVI surface and price to finite values at 4,096 paths. Fitted values,
// violation counts and prices are reported by aadp_essvi_report, not asserted here. The files are
// never committed. With no file the tests skip, visibly, and say how to get it.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/runner.hpp"
#include "svi/arbitrage.hpp"
#include "svi/chain_essvi.hpp"
#include "svi/chain_fit.hpp"
#include "svi/chain_surface.hpp"
#include "svi/essvi_fit.hpp"
#include "svi/fit.hpp"
#include "svi/surface.hpp"

namespace aadp::svi {
namespace {

namespace fs = std::filesystem;

// Development seed, never 1729 or 271828: 7a's pricing seed.
constexpr std::uint64_t kSeed = 20261115;
constexpr std::uint64_t kPaths = 4096;

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

std::optional<std::vector<ChainSlice>> Slices(const std::string& option_bytes,
                                              const std::string& future_bytes) {
  const auto options = taifex::read_options(option_bytes);
  const auto futures = taifex::read_futures(future_bytes);
  EXPECT_TRUE(options.has_value() && futures.has_value());
  if (!options || !futures) return std::nullopt;
  auto slices = BuildChainSlices(options->rows, futures->rows, kCalibrationExpiries);
  EXPECT_TRUE(slices.has_value());
  if (!slices) return std::nullopt;
  return std::move(*slices);
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

TEST(RealEssvi, TheSurfaceFitsConvergesAndHoldsEveryConstraint) {
  AADP_LOAD_OR_SKIP(option_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(future_bytes, kFuturesFile);
  const auto slices = Slices(*option_bytes, *future_bytes);
  if (!slices) GTEST_FAIL() << "the chain did not build";
  const EssviChainFit chain = FitChainEssvi(*slices);
  ASSERT_TRUE(chain.fit.has_value()) << EssviFitErrorName(chain.fit.error().code);
  const EssviFitResult& r = *chain.fit;
  ASSERT_EQ(r.slices.size(), 3U);
  EXPECT_TRUE(r.converged) << EssviStopName(r.stop);
  EXPECT_EQ(r.constraints.violations, 0U);
  double total = 0.0;
  for (std::size_t j = 0; j < r.slices.size(); ++j) {
    EXPECT_TRUE(CheckParameters(r.slices[j].raw).empty()) << "slice " << j;
    const double phi6b = Objective((*slices)[j].quotes, r.slices[j].raw);
    EXPECT_NEAR(r.slices[j].objective, phi6b,
                (1e-9 * phi6b) + ObjectiveRounding((*slices)[j].quotes, r.slices[j].residual))
        << "slice " << j;
    EXPECT_TRUE(std::isfinite(r.slices[j].objective)) << "slice " << j;
    total += r.slices[j].objective;
  }
  EXPECT_NEAR(r.objective, total, 1e-12 * total);
  // The checks run: a grid was evaluated for every slice and every adjacent pair.
  ASSERT_EQ(chain.slices.size(), 3U);
  for (const EssviChainSlice& s : chain.slices) EXPECT_GT(s.butterfly.grid_points, 0U);
  ASSERT_EQ(chain.calendar.size(), 2U);
  for (const CalendarPair& p : chain.calendar) EXPECT_GT(p.report.grid_points, 0U);
}

TEST(RealEssvi, TheContractsValidateAndPriceToFiniteValuesOnTheEssviSurface) {
  AADP_LOAD_OR_SKIP(option_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(future_bytes, kFuturesFile);
  const auto slices = Slices(*option_bytes, *future_bytes);
  if (!slices) GTEST_FAIL() << "the chain did not build";
  const std::optional<ChainSurface> surface = SurfaceFromEssvi(*slices, FitChainEssvi(*slices));
  if (!surface) GTEST_FAIL() << "the eSSVI surface did not build";
  const ChainSurface* s = &*surface;
  ASSERT_EQ(s->terms.size(), 3U);
  EXPECT_EQ(ValidateSurface(s->terms, s->params), SurfaceError::kNone);
  const pricer::RunConfig run{.seed = kSeed, .paths = kPaths, .batches = 64, .threads = 2};
  for (const NamedContract& c : SurfaceContracts(s->terms.back().expiry)) {
    EXPECT_EQ(pricer::ValidateLocalVol(c.contract, s->terms, s->params, {}).code,
              pricer::LocalVolError::kNone)
        << c.name;
    const auto price = pricer::PriceLocalVol(c.contract, s->terms, s->params, run);
    ASSERT_TRUE(price.has_value()) << c.name;
    EXPECT_TRUE(std::isfinite(price->value.mean)) << c.name;
    EXPECT_TRUE(std::isfinite(price->value.standard_error)) << c.name;
  }
}

}  // namespace
}  // namespace aadp::svi
