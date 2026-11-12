// R10 to R12 of the QuantLib comparison: speed and sanity rows under local volatility, never
// judged for accuracy (docs/NOTES.md, "Stage 10").
//
// The surface is synthetic and written here, so no market data is involved: raw SVI slices at
// T = 0.25, 0.5 and 1 with w_j(k) = T_j (0.02 + 0.1 (rho (k - m) + sqrt((k - m)^2 + s^2))),
// rho = -0.5, m = 0, s = 0.2 (20% at the money), on S0 = 100, r = 0.10, q = 0.
//
// R10 and R11 read one local-volatility matrix, this project's Dupire (svi/local_vol.hpp) on that
// surface at 51 times and 111 spots, through QuantLib's FixedLocalVolSurface rule, which GridVol
// restates: at a time on the grid, linear in spot with linear extrapolation; between two grid
// times, each neighbour linear in spot with the spot held inside the grid, then linear in local
// volatility in time; times clamped to the grid (quantlib-compare/EVIDENCE.md, E21).
#ifndef AADP_BENCH_QUANTLIB_LOCAL_VOL_ROWS_HPP_
#define AADP_BENCH_QUANTLIB_LOCAL_VOL_ROWS_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "pricer/contract.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::qlcompare {

inline constexpr std::uint64_t kLocalVolPaths = std::uint64_t{1} << 18;

struct SyntheticSurface {
  double spot = 100.0;
  double r = 0.10;
  double q = 0.0;
  std::vector<double> days;  // Actual360 days to each expiry, for QuantLib's dates
  std::vector<svi::SliceTerms> terms;
  std::vector<svi::RawSvi<double>> params;
};
SyntheticSurface MakeSyntheticSurface();

struct LocalVolGrid {
  std::vector<double> times;  // increasing
  std::vector<double> spots;  // increasing
  std::vector<double> vols;   // vols[time * spots.size() + spot]
  [[nodiscard]] double At(std::size_t time, std::size_t spot) const {
    return vols[time * spots.size() + spot];
  }
  // QuantLib's FixedLocalVolSurface rule, restated above.
  [[nodiscard]] double Vol(double t, double s) const;
};

// QuantLib's close_enough(x, y), n = 42: equal within 42 epsilon relative to either.
bool CloseEnough(double x, double y);

// sqrt of this project's local variance, after its floor rule, at t = 0, 0.02, ..., 1 and
// S = 30, 32, ..., 250.
std::expected<LocalVolGrid, std::string> BuildGrid(const SyntheticSurface& surface);

// Log-Euler with the left point's grid volatility, as QuantLib's 5-argument process steps with an
// external local volatility (EVIDENCE E18): mean x + (r - q - sigma^2 / 2) dt, sd sigma sqrt(dt).
class GridModel {
 public:
  GridModel(const LocalVolGrid& grid, const pricer::Market<double>& market, std::uint32_t steps);
  [[nodiscard]] const double& Spot() const { return spot_; }
  [[nodiscard]] const double& LogSpot() const { return log_spot_; }
  [[nodiscard]] const double& Discount() const { return discount_; }
  [[nodiscard]] pricer::StepLaw<double> Law(std::uint32_t step, const double& x) const;

 private:
  const LocalVolGrid* grid_;
  double spot_;
  double log_spot_;
  double discount_;
  double carry_;
  double dt_;
  double sqrt_dt_;
};

enum class LocalVolRow : std::uint8_t { kR10, kR11 };
// R10: R7's down-and-out call at H 95, discrete at 50 dates, hard indicator, T 0.2, r 0.10.
// R11: R4's arithmetic Asian call, 51 points, T 1, r 0.0367. sigma is unused on the grid.
pricer::Contract LocalVolRowContract(LocalVolRow row);
pricer::Market<double> LocalVolRowMarket(LocalVolRow row);

std::expected<pricer::Estimate, std::string> PriceOnGrid(LocalVolRow row, const LocalVolGrid& grid,
                                                         const pricer::RunConfig& config);

// R12: a European call, K 100, T 1 (the last slice), 50 Euler steps, under this project's local
// volatility off the surface, and its sanity reference, the Black price at the slice's implied
// volatility.
inline constexpr double kR12Strike = 100.0;
inline constexpr std::uint32_t kR12Steps = 50;
std::expected<pricer::Estimate, std::string> PriceR12(const SyntheticSurface& surface,
                                                      const pricer::RunConfig& config);
double R12Reference(const SyntheticSurface& surface);

}  // namespace aadp::qlcompare

#endif  // AADP_BENCH_QUANTLIB_LOCAL_VOL_ROWS_HPP_
