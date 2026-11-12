// Stage 8's first steps on the fitted chain (docs/NOTES.md, "Stage 8"): each of the 15 h0 against
// the closed form, M taken on k = -0.5, -0.499, ..., 0.5 and not over the quoted range, and each
// step inside the validated domain on both sides. Skips, visibly, without the captures.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "ad/local_vol_gradient.hpp"
#include "ladder.hpp"
#include "production.hpp"
#include "svi/raw_svi.hpp"

namespace aadp::oracle_runner {
namespace {

namespace fs = std::filesystem;

fs::path DataDir() {
  const char* override_dir = std::getenv("AADP_DATA_DIR");
  return override_dir != nullptr ? fs::path(override_dir) : fs::path("data");
}

// The closed forms of docs/NOTES.md, "Stage 8", written out again here.
double Slope(const svi::RawSvi<double>& p, std::size_t parameter, double k) {
  const double x = k - p.m;
  const double r = std::hypot(x, p.sigma);
  const std::array<double, 5> slopes = {1.0, (p.rho * x) + r, p.b * x, -p.b * (p.rho + (x / r)),
                                        p.b * p.sigma / r};
  return slopes[parameter];
}

TEST(SviRowsRealData, FirstStepsMatchTheClosedFormAndStayInTheDomain) {
  for (const char* name : {"DailyMarketReportOpt.csv", "DailyMarketReportFut.csv"}) {
    if (!fs::exists(DataDir() / name)) {
      GTEST_SKIP() << "SKIPPED: " << (DataDir() / name).string()
                   << " is absent, so this real-file test did not run. Fetch it with "
                      "scripts/fetch_data.sh.";
    }
  }
  const auto surface = LoadChainSurface(DataDir().string());
  ASSERT_TRUE(surface.has_value()) << surface.error();
  ASSERT_EQ(surface->params.size(), 3U);
  for (std::size_t j = 0; j < surface->params.size(); ++j) {
    const svi::RawSvi<double>& p = surface->params[j];
    const double w0 = p.a + (p.b * (std::hypot(p.m, p.sigma) - (p.rho * p.m)));
    for (std::size_t i = 0; i < ad::kParametersPerSlice; ++i) {
      double largest = 0.0;
      for (int n = 0; n <= 1000; ++n) {
        largest = std::max(largest, std::fabs(Slope(p, i, -0.5 + (0.001 * n))));
      }
      const double expected = w0 / (256.0 * largest);
      const double h0 = FirstStep(p, i);
      std::cout << "slice " << j << " parameter " << i << ": h0 " << h0 << '\n';
      EXPECT_NEAR(h0, expected, 1e-12 * expected) << "slice " << j << " parameter " << i;
      const auto outside = StepOutsideDomain(p, i, h0);
      EXPECT_FALSE(outside.has_value())
          << "slice " << j << " parameter " << i << ": " << outside.value_or("");
    }
  }
}

}  // namespace
}  // namespace aadp::oracle_runner
