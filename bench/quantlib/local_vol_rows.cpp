#include "quantlib/local_vol_rows.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "rng/normal.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::qlcompare {

namespace {

constexpr std::size_t kGridTimes = 51;   // 0, 0.02, ..., 1
constexpr std::size_t kGridSpots = 111;  // 30, 32, ..., 250

// Linear interpolation in spot on one time column, extrapolating the end segments.
double LinearInSpot(const LocalVolGrid& grid, std::size_t time, double s) {
  const std::vector<double>& x = grid.spots;
  const std::size_t n = x.size();
  std::size_t i = 0;
  if (s > x[n - 1]) {
    i = n - 2;
  } else if (s >= x[0]) {
    const auto it = std::upper_bound(x.begin(), x.end() - 1, s);
    i = std::min(static_cast<std::size_t>(it - x.begin()) - 1, n - 2);
  }
  const double y0 = grid.At(time, i);
  const double slope = (grid.At(time, i + 1) - y0) / (x[i + 1] - x[i]);
  return y0 + (s - x[i]) * slope;
}

pricer::Estimate FromTable(const std::vector<std::array<double, 1>>& table) {
  pricer::Estimate estimate;
  estimate.batch_means.reserve(table.size());
  for (const auto& row : table) estimate.batch_means.push_back(row[0]);
  const BatchStatistics stats = BatchMeansStatistics(estimate.batch_means);
  estimate.mean = stats.mean;
  estimate.standard_error = stats.standard_error;
  return estimate;
}

}  // namespace

SyntheticSurface MakeSyntheticSurface() {
  SyntheticSurface s;
  s.days = {90.0, 180.0, 360.0};
  for (const double days : s.days) {
    const double t = days / 360.0;
    s.terms.push_back({.expiry = t,
                       .forward = s.spot * std::exp((s.r - s.q) * t),
                       .discount = std::exp(-s.r * t)});
    s.params.push_back({.a = 0.02 * t, .b = 0.1 * t, .rho = -0.5, .m = 0.0, .sigma = 0.2});
  }
  return s;
}

bool CloseEnough(double x, double y) {
  if (x == y) return true;
  const double diff = std::fabs(x - y);
  const double tolerance = 42.0 * DBL_EPSILON;
  if (x * y == 0.0) return diff < tolerance * tolerance;
  return diff <= tolerance * std::fabs(x) || diff <= tolerance * std::fabs(y);
}

double LocalVolGrid::Vol(double t, double s) const {
  t = std::min(times.back(), std::max(t, times.front()));
  const auto idx = static_cast<std::size_t>(std::ranges::lower_bound(times, t) - times.begin());
  if (CloseEnough(t, times[idx])) return LinearInSpot(*this, idx, s);
  const double held = std::clamp(s, spots.front(), spots.back());
  const double early = LinearInSpot(*this, idx - 1, held);
  const double later = LinearInSpot(*this, idx, held);
  return early + (later - early) / (times[idx] - times[idx - 1]) * (t - times[idx - 1]);
}

std::expected<LocalVolGrid, std::string> BuildGrid(const SyntheticSurface& surface) {
  const svi::SurfaceError error = svi::ValidateSurface(surface.terms, surface.params);
  if (error != svi::SurfaceError::kNone) return std::unexpected(svi::SurfaceErrorName(error));
  const svi::SviSurface<double> svi_surface(surface.terms, surface.params);
  const svi::LocalVolRule rule{};
  LocalVolGrid grid;
  for (std::size_t i = 0; i < kGridTimes; ++i) grid.times.push_back(0.02 * static_cast<double>(i));
  for (std::size_t j = 0; j < kGridSpots; ++j) {
    grid.spots.push_back(30.0 + 2.0 * static_cast<double>(j));
  }
  grid.vols.reserve(kGridTimes * kGridSpots);
  for (const double t : grid.times) {
    const svi::TimeSlot slot = svi_surface.Locate(t);
    const double log_forward = svi_surface.Curve().LogForward(t);
    for (const double spot : grid.spots) {
      const double k = std::log(spot) - log_forward;
      const svi::LocalVariance<double> lv = svi::LocalVarianceAt(svi_surface.At(slot, k), k, rule);
      grid.vols.push_back(std::sqrt(lv.variance));
    }
  }
  return grid;
}

GridModel::GridModel(const LocalVolGrid& grid, const pricer::Market<double>& market,
                     std::uint32_t steps)
    : grid_(&grid),
      spot_(market.spot),
      log_spot_(std::log(market.spot)),
      discount_(std::exp(-market.r * market.maturity)),
      carry_(market.r - market.q),
      dt_(market.maturity / static_cast<double>(steps)),
      sqrt_dt_(std::sqrt(dt_)) {}

pricer::StepLaw<double> GridModel::Law(std::uint32_t step, const double& x) const {
  const double sigma = grid_->Vol(static_cast<double>(step) * dt_, std::exp(x));
  return {.mean = x + (carry_ - 0.5 * sigma * sigma) * dt_, .sd = sigma * sqrt_dt_};
}

pricer::Contract LocalVolRowContract(LocalVolRow row) {
  pricer::Contract c;
  c.strike = 100.0;
  c.steps = 50;
  if (row == LocalVolRow::kR11) {
    c.product = pricer::Product::kAsianArithmetic;
    c.include_spot = true;
    return c;
  }
  c.product = pricer::Product::kBarrier;
  c.barrier = {.direction = pricer::Direction::kDown,
               .knock = pricer::Knock::kOut,
               .monitoring = pricer::Monitoring::kDiscrete,
               .treatment = pricer::Treatment::kHard,
               .level = 95.0};
  return c;
}

pricer::Market<double> LocalVolRowMarket(LocalVolRow row) {
  if (row == LocalVolRow::kR11) {
    return {.spot = 100.0, .sigma = 0.2, .r = 0.0367, .q = 0.0, .maturity = 1.0};
  }
  return {.spot = 100.0, .sigma = 0.2, .r = 0.10, .q = 0.0, .maturity = 0.2};
}

std::expected<pricer::Estimate, std::string> PriceOnGrid(LocalVolRow row, const LocalVolGrid& grid,
                                                         const pricer::RunConfig& config) {
  const pricer::Contract contract = LocalVolRowContract(row);
  const pricer::Market<double> market = LocalVolRowMarket(row);
  const pricer::ContractError error = pricer::Validate(contract, market);
  if (error != pricer::ContractError::kNone) return std::unexpected(pricer::ErrorName(error));
  if (pricer::ValidateRun(config) != pricer::RunError::kNone) {
    return std::unexpected("bad run configuration");
  }
  const GridModel model(grid, market, contract.steps);
  const pricer::PathShape shape{.steps = contract.steps, .crossing = false};
  return FromTable(pricer::RunBatches<1>(config, shape, [&] {
    return [&](const pricer::PathUniforms& u, std::array<double, 1>& out) {
      out[0] = pricer::DiscountedPayoff<double>(contract, model, u);
    };
  }));
}

std::expected<pricer::Estimate, std::string> PriceR12(const SyntheticSurface& surface,
                                                      const pricer::RunConfig& config) {
  pricer::Contract european;
  european.product = pricer::Product::kEuropean;
  european.strike = kR12Strike;
  const pricer::LocalVolContract lv{
      .contract = european, .maturity = surface.terms.back().expiry, .substeps = kR12Steps};
  auto result = pricer::PriceLocalVol(lv, surface.terms, surface.params, config);
  if (!result) return std::unexpected(pricer::LocalVolErrorName(result.error().code));
  return std::move(result->value);
}

double R12Reference(const SyntheticSurface& surface) {
  const svi::SliceTerms& last = surface.terms.back();
  const double k = std::log(kR12Strike / last.forward);
  const double w = svi::TotalVariance(surface.params.back(), k);
  const double sd = std::sqrt(w);
  const double d1 = (-k + 0.5 * w) / sd;
  const double d2 = d1 - sd;
  return last.discount * (last.forward * normal_cdf(d1) - kR12Strike * normal_cdf(d2));
}

}  // namespace aadp::qlcompare
