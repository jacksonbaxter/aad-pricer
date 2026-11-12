// The chain into the fit on the pinned captures. Asserts only what docs/NOTES.md, "Stage 6b", fixed
// before the first fit: the slices, their sizes and input counts, that every fit returns, and that
// the checks ran. The fitted values are reported by aadp_svi_fit, not asserted here. The files are
// never committed. With no file the tests skip, visibly, and say how to get it. The sizes and
// counts asserted here are derived from the chain. Data: TAIFEX via data.gov.tw, Open Government
// Data License (NOTICE).

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
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

// docs/NOTES.md, "Recommended calibration set" and the stage 6b pre-registration.
constexpr std::array<std::size_t, 3> kRows = {65, 51, 64};
constexpr std::array<std::size_t, 3> kTriples = {61, 47, 60};
constexpr std::array<std::size_t, 3> kNonConvex = {15, 19, 17};
constexpr std::array<std::size_t, 3> kBeyondHalfTick = {3, 13, 7};

TEST(RealChainFit, SlicesHaveThePublishedSizesCountsAndWeights) {
  AADP_LOAD_OR_SKIP(option_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(future_bytes, kFuturesFile);
  const auto options = taifex::read_options(*option_bytes);
  const auto futures = taifex::read_futures(*future_bytes);
  ASSERT_TRUE(options.has_value());
  ASSERT_TRUE(futures.has_value());
  const auto slices = BuildChainSlices(options->rows, futures->rows, kCalibrationExpiries);
  ASSERT_TRUE(slices.has_value()) << ChainErrorName(slices.error().code);
  ASSERT_EQ(slices->size(), 3U);
  for (std::size_t i = 0; i < 3; ++i) {
    const ChainSlice& s = (*slices)[i];
    EXPECT_EQ(s.expiry, kCalibrationExpiries[i]);
    EXPECT_EQ(s.quotes.quotes.size(), kRows[i]) << s.expiry;
    EXPECT_EQ(s.input_butterflies.triples, kTriples[i]) << s.expiry;
    EXPECT_EQ(s.input_butterflies.non_convex, kNonConvex[i]) << s.expiry;
    EXPECT_EQ(s.input_butterflies.beyond_half_tick, kBeyondHalfTick[i]) << s.expiry;
    for (const FitQuote& q : s.quotes.quotes) {
      EXPECT_GE(q.noise, s.parity.sigma_leg) << s.expiry;
      EXPECT_TRUE(std::isfinite(q.noise) && q.noise > 0.0) << s.expiry;
    }
  }
  // The weekly has no TX month.
  EXPECT_TRUE((*slices)[0].tx_settlement.has_value());
  EXPECT_FALSE((*slices)[1].tx_settlement.has_value());
  EXPECT_TRUE((*slices)[2].tx_settlement.has_value());
}

TEST(RealChainFit, EverySliceFitsAndEveryCheckRuns) {
  AADP_LOAD_OR_SKIP(option_bytes, kOptionsFile);
  AADP_LOAD_OR_SKIP(future_bytes, kFuturesFile);
  const auto options = taifex::read_options(*option_bytes);
  const auto futures = taifex::read_futures(*future_bytes);
  ASSERT_TRUE(options.has_value());
  ASSERT_TRUE(futures.has_value());
  const auto slices = BuildChainSlices(options->rows, futures->rows, kCalibrationExpiries);
  ASSERT_TRUE(slices.has_value());
  const SurfaceFit surface = FitChain(*slices);
  ASSERT_EQ(surface.slices.size(), 3U);
  for (std::size_t i = 0; i < 3; ++i) {
    const SliceFit& r = surface.slices[i];
    ASSERT_TRUE(r.fit.has_value())
        << kCalibrationExpiries[i] << ' ' << FitErrorName(r.fit.error().code);
    EXPECT_EQ(r.fit->quotes, kRows[i]);
    EXPECT_TRUE(std::isfinite(r.fit->objective));
    EXPECT_TRUE(std::isfinite(r.svi6_bp));
    EXPECT_TRUE(std::isfinite(r.svi11_vol_points));
    EXPECT_GT(r.butterfly.grid_points, 0U);
    EXPECT_EQ(r.model_butterflies.triples, kTriples[i]);
  }
  ASSERT_EQ(surface.calendar.size(), 2U);
  for (const CalendarPair& pair : surface.calendar) {
    EXPECT_EQ(pair.later, pair.earlier + 1);
    EXPECT_GT(pair.report.grid_points, 0U);
  }
}

}  // namespace
}  // namespace aadp::svi
