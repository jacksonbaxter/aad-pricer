#include "data/forwards.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "data/calibration_set.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"

namespace aadp {

namespace {

struct ExpiryDate {
  std::string_view expiry;
  std::int32_t date;
};

constexpr std::array<ExpiryDate, 6> kTxoExpiries = {{{.expiry = "202411", .date = 20241120},
                                                     {.expiry = "202411W4", .date = 20241127},
                                                     {.expiry = "202412", .date = 20241218},
                                                     {.expiry = "202501", .date = 20250115},
                                                     {.expiry = "202503", .date = 20250319},
                                                     {.expiry = "202506", .date = 20250618}}};

std::chrono::sys_days to_days(std::int32_t yyyymmdd) {
  const std::chrono::year_month_day day{
      std::chrono::year{yyyymmdd / 10000},
      std::chrono::month{static_cast<unsigned>(yyyymmdd / 100 % 100)},
      std::chrono::day{static_cast<unsigned>(yyyymmdd % 100)}};
  return std::chrono::sys_days{day};
}

}  // namespace

std::vector<ParityPoint> settlement_parity_points(std::span<const taifex::OptionRow> rows,
                                                  std::string_view expiry) {
  std::map<double, std::pair<std::optional<double>, std::optional<double>>> by_strike;
  for (const taifex::OptionRow& row : rows) {
    if (row.contract != kCalibrationContract || row.expiry != expiry ||
        row.session != taifex::Session::kRegular || !row.settlement_price.has_value()) {
      continue;
    }
    auto& legs = by_strike[row.strike];
    (row.right == taifex::OptionRight::kCall ? legs.first : legs.second) = *row.settlement_price;
  }
  std::vector<ParityPoint> points;
  for (const auto& [strike, legs] : by_strike) {
    if (legs.first && legs.second) {
      points.push_back({.strike = strike, .call_minus_put = *legs.first - *legs.second});
    }
  }
  return points;
}

std::expected<ParityFit, ParityError> fit_parity(std::span<const ParityPoint> points) {
  const std::size_t n = points.size();
  if (n < 3) {
    return std::unexpected(ParityError::kTooFewPoints);
  }
  if (!std::ranges::all_of(points, [](const ParityPoint& p) {
        return std::isfinite(p.strike) && std::isfinite(p.call_minus_put);
      })) {
    return std::unexpected(ParityError::kNonFinite);
  }
  // Centred sums, in the order of Phase 1's script.
  const auto count = static_cast<double>(n);
  double sum_x = 0.0;
  double sum_y = 0.0;
  for (const ParityPoint& p : points) {
    sum_x += p.strike;
    sum_y += p.call_minus_put;
  }
  const double mean_x = sum_x / count;
  const double mean_y = sum_y / count;
  double sxx = 0.0;
  double sxy = 0.0;
  for (const ParityPoint& p : points) {
    sxx += (p.strike - mean_x) * (p.strike - mean_x);
  }
  for (const ParityPoint& p : points) {
    sxy += (p.strike - mean_x) * (p.call_minus_put - mean_y);
  }
  if (!(sxx > 0.0) || !std::isfinite(sxx)) {
    return std::unexpected(ParityError::kNoStrikeSpread);
  }
  const double slope = sxy / sxx;
  const double intercept = mean_y - (slope * mean_x);
  if (!(slope < 0.0)) {
    return std::unexpected(ParityError::kNonPositiveDiscount);
  }
  double sum_squares = 0.0;
  double max_abs = 0.0;
  for (const ParityPoint& p : points) {
    const double residual = p.call_minus_put - intercept - (slope * p.strike);
    sum_squares += residual * residual;
    max_abs = std::max(max_abs, std::abs(residual));
  }
  const double discount = -slope;
  const double forward = intercept / discount;
  const double s2 = sum_squares / (count - 2.0);
  const double var_slope = s2 / sxx;
  const double var_intercept = s2 * ((1.0 / count) + (mean_x * mean_x / sxx));
  const double cov = -mean_x * var_slope;
  const double var_forward =
      (var_intercept + (forward * forward * var_slope) + (2.0 * forward * cov)) / (slope * slope);
  const double rms = std::sqrt(sum_squares / count);
  return ParityFit{.points = n,
                   .forward = forward,
                   .discount = discount,
                   .forward_se = std::sqrt(std::max(var_forward, 0.0)),
                   .discount_se = std::sqrt(std::max(var_slope, 0.0)),
                   .rms_residual = rms,
                   .sigma_leg = rms / std::numbers::sqrt2,
                   .max_abs_residual = max_abs};
}

std::optional<double> tx_settlement(std::span<const taifex::FuturesRow> rows,
                                    std::string_view expiry) {
  for (const taifex::FuturesRow& row : rows) {
    if (row.contract == "TX" && !row.calendar_spread && row.expiry == expiry &&
        row.session == taifex::Session::kRegular && row.settlement_price.has_value()) {
      return row.settlement_price;
    }
  }
  return std::nullopt;
}

std::optional<std::int32_t> txo_expiry_date(std::string_view expiry) {
  const auto* found = std::ranges::find(kTxoExpiries, expiry, &ExpiryDate::expiry);
  if (found == kTxoExpiries.end()) {
    return std::nullopt;
  }
  return found->date;
}

std::optional<std::int64_t> txo_seconds_to_expiry(std::string_view expiry) {
  const auto date = txo_expiry_date(expiry);
  if (!date) {
    return std::nullopt;
  }
  using std::chrono::minutes;
  using std::chrono::seconds;
  const auto start = to_days(kSnapshotTradeDate) + minutes{kSnapshotMinutes};
  const auto end = to_days(*date) + minutes{kExpiryMinutes};
  return std::chrono::duration_cast<seconds>(end - start).count();
}

std::optional<double> txo_years_to_expiry(std::string_view expiry) {
  const auto secs = txo_seconds_to_expiry(expiry);
  if (!secs) {
    return std::nullopt;
  }
  return static_cast<double>(*secs) / kSecondsPerYear;
}

}  // namespace aadp
