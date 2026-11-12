// The calibration adjoint on the chain fitted to the pinned captures. Asserts only what
// docs/NOTES.md, "Stage 7b", fixed before the first real-surface sensitivity: every slice's adjoint
// is computed (no refusal), one multiplier per active bound, positive and clear of its numerical
// uncertainty, every derivative finite and on the constraint manifold, and the middle quote of
// each slice passes the bump-and-recalibrate check on its parameters with the market held. The
// sensitivities themselves are reported by aadp_quote_sensitivity_report, not asserted here. The
// files are never committed. With no file the tests skip, visibly, and say how to get it.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "data/forwards.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
#include "svi/arbitrage.hpp"
#include "svi/calibration_adjoint.hpp"
#include "svi/chain_adjoint.hpp"
#include "svi/chain_fit.hpp"
#include "svi/fit.hpp"

namespace aadp::svi {
namespace {

namespace fs = std::filesystem;

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

Vector5 AsVector(const SviParams& p) { return {p.a, p.b, p.rho, p.m, p.sigma}; }

bool AllFinite(const Vector5& v) {
  return std::ranges::all_of(v, [](double x) { return std::isfinite(x); });
}

struct Chain {
  std::vector<taifex::OptionRow> options;
  std::vector<ChainSlice> slices;
  SurfaceFit fit;
};

std::optional<Chain> Fitted(const std::string& option_bytes, const std::string& future_bytes) {
  const auto options = taifex::read_options(option_bytes);
  const auto futures = taifex::read_futures(future_bytes);
  EXPECT_TRUE(options.has_value() && futures.has_value());
  if (!options || !futures) return std::nullopt;
  auto slices = BuildChainSlices(options->rows, futures->rows, kCalibrationExpiries);
  EXPECT_TRUE(slices.has_value());
  if (!slices) return std::nullopt;
  Chain out{.options = options->rows, .slices = std::move(*slices), .fit = {}};
  out.fit = FitChain(out.slices);
  return out;
}

TEST(RealQuoteSensitivity, EverySliceHasItsAdjointOnTheManifold) {
  AADP_LOAD_OR_SKIP(option_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(future_bytes, kFuturesFile);
  const std::optional<Chain> chain = Fitted(*option_bytes, *future_bytes);
  if (!chain) GTEST_FAIL() << "the chain did not build";
  ASSERT_EQ(chain->slices.size(), 3U);
  const std::array<std::size_t, 3> quotes = {65, 51, 64};
  for (std::size_t j = 0; j < 3; ++j) {
    const ChainSlice& slice = chain->slices[j];
    ASSERT_TRUE(chain->fit.slices[j].fit.has_value()) << slice.expiry;
    const FitResult& fit = *chain->fit.slices[j].fit;
    ASSERT_TRUE(fit.converged) << slice.expiry;
    ASSERT_EQ(slice.quotes.quotes.size(), quotes[j]) << slice.expiry;
    const auto points = settlement_parity_points(chain->options, slice.expiry);
    const auto s = SliceQuoteSensitivities(slice, points, fit);
    ASSERT_TRUE(s.has_value()) << slice.expiry << ": " << AdjointErrorName(s.error().adjoint.code)
                               << " " << s.error().adjoint.value;
    const CalibrationAdjoint& a = s->adjoint;
    const std::vector<Bound> active = fit.ActiveBounds();
    ASSERT_EQ(a.constraints.size(), active.size()) << slice.expiry;
    for (const ActiveConstraint& c : a.constraints) {
      EXPECT_GT(c.multiplier, 0.0) << slice.expiry;
      EXPECT_GE(c.margin, AdjointOptions{}.min_multiplier_margin) << slice.expiry;
      EXPECT_TRUE(std::isfinite(c.significance) && std::isfinite(c.quote_distance)) << slice.expiry;
    }
    EXPECT_LE(a.exact.condition, AdjointOptions{}.max_condition) << slice.expiry;
    EXPECT_LE(a.exact.manifold_residual, 0x1p-40) << slice.expiry;
    ASSERT_EQ(a.exact.quote.size(), quotes[j]);
    ASSERT_EQ(s->through_parity.size(), quotes[j]);
    for (std::size_t i = 0; i < quotes[j]; ++i) {
      EXPECT_TRUE(AllFinite(a.exact.quote[i]) && AllFinite(s->through_parity[i]))
          << slice.expiry << " quote " << i;
    }
  }
}

TEST(RealQuoteSensitivity, TheMiddleQuotePassesTheBumpCheck) {
  AADP_LOAD_OR_SKIP(option_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(future_bytes, kFuturesFile);
  const std::optional<Chain> chain = Fitted(*option_bytes, *future_bytes);
  if (!chain) GTEST_FAIL() << "the chain did not build";
  for (std::size_t j = 0; j < chain->slices.size(); ++j) {
    const ChainSlice& slice = chain->slices[j];
    ASSERT_TRUE(chain->fit.slices[j].fit.has_value());
    const FitResult& fit = *chain->fit.slices[j].fit;
    const auto points = settlement_parity_points(chain->options, slice.expiry);
    const auto s = SliceQuoteSensitivities(slice, points, fit);
    ASSERT_TRUE(s.has_value()) << slice.expiry;
    // The refit error, from the two alternative grids.
    Vector5 refit{};
    FitOptions alt1;
    alt1.grid_m = 8;
    FitOptions alt2;
    alt2.grid_sigma = 8;
    for (const FitOptions& alt : {alt1, alt2}) {
      const auto f = FitRawSvi(slice.quotes, alt);
      ASSERT_TRUE(f.has_value());
      const Vector5 got = AsVector(f->params);
      const Vector5 want = AsVector(fit.params);
      for (std::size_t p = 0; p < 5; ++p) refit[p] = std::max(refit[p], std::abs(got[p] - want[p]));
    }
    const std::size_t i = slice.quotes.quotes.size() / 2;
    const FitQuote& q = slice.quotes.quotes[i];
    const std::vector<Bound> active = fit.ActiveBounds();
    constexpr std::size_t kLevels = 6;
    std::array<double, kLevels> h{};
    std::array<bool, kLevels> usable{};
    std::array<Vector5, kLevels> difference{};
    for (std::size_t l = 0; l < kLevels; ++l) {
      h[l] = std::ldexp(std::min(q.noise, 0.5 * q.price), -static_cast<int>(l));
      const auto up = BumpQuote(slice, points, s->legs, i, h[l], QuoteChain::kMarketFixed);
      const auto down = BumpQuote(slice, points, s->legs, i, -h[l], QuoteChain::kMarketFixed);
      ASSERT_TRUE(up.has_value() && down.has_value());
      const auto fu = FitRawSvi(up->quotes);
      const auto fd = FitRawSvi(down->quotes);
      usable[l] = fu.has_value() && fd.has_value() && fu->converged && fd->converged &&
                  fu->ActiveBounds() == active && fd->ActiveBounds() == active;
      if (!usable[l]) continue;
      const Vector5 pu = AsVector(fu->params);
      const Vector5 pd = AsVector(fd->params);
      for (std::size_t p = 0; p < 5; ++p) difference[l][p] = (pu[p] - pd[p]) / (2.0 * h[l]);
    }
    const double kappa = s->adjoint.exact.condition;
    for (std::size_t p = 0; p < 5; ++p) {
      double largest = 0.0;
      for (const Vector5& d : s->adjoint.exact.quote) largest = std::max(largest, std::abs(d[p]));
      std::array<double, kLevels> column{};
      for (std::size_t l = 0; l < kLevels; ++l) column[l] = difference[l][p];
      const HalvingJudgement e = JudgeByHalving(
          h, column, usable, refit[p], s->adjoint.exact.quote[i][p], kappa * 0x1p-52 * largest);
      EXPECT_TRUE(e.resolved && e.pass)
          << slice.expiry << " quote " << i << " parameter " << p << ": error " << e.error
          << ", tolerance " << e.tolerance << ", level " << e.level;
    }
  }
}

}  // namespace
}  // namespace aadp::svi
