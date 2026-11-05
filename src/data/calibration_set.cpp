#include "data/calibration_set.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <iterator>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "data/taifex_csv.hpp"
#include "data/taifex_options.hpp"

namespace aadp {

namespace {

bool is_out_of_the_money(const taifex::OptionRow& row, double forward) {
  return row.right == taifex::OptionRight::kPut ? row.strike < forward : row.strike > forward;
}

bool passes_filters(const taifex::OptionRow& row, std::string_view expiry, double forward) {
  // Equality on doubles is exact here: both sides come from parse_decimal, which gives equal
  // doubles for equal decimal values.
  return row.contract == kCalibrationContract && row.expiry == expiry &&
         row.session == taifex::Session::kRegular && is_out_of_the_money(row, forward) &&
         row.volume.has_value() && *row.volume > 0 && row.settlement_price.has_value() &&
         row.last_trade_price.has_value() && *row.settlement_price == *row.last_trade_price &&
         *row.settlement_price >= kMinSettlementPoints;
}

}  // namespace

std::expected<std::vector<CalibrationSlice>, CalibrationError> select_calibration_set(
    std::span<const taifex::OptionRow> rows, std::span<const ExpiryForward> forwards) {
  for (auto it = forwards.begin(); it != forwards.end(); ++it) {
    if (!std::isfinite(it->forward) || it->forward <= 0.0) {
      return std::unexpected(CalibrationError::kBadForward);
    }
    if (std::any_of(forwards.begin(), it,
                    [&](const ExpiryForward& other) { return other.expiry == it->expiry; })) {
      return std::unexpected(CalibrationError::kDuplicateExpiry);
    }
  }

  std::vector<CalibrationSlice> slices;
  slices.reserve(forwards.size());
  for (const ExpiryForward& entry : forwards) {
    CalibrationSlice slice{.expiry = entry.expiry, .forward = entry.forward, .rows = {}};
    std::ranges::copy_if(rows, std::back_inserter(slice.rows), [&](const taifex::OptionRow& row) {
      return passes_filters(row, entry.expiry, entry.forward);
    });
    slices.push_back(std::move(slice));
  }
  return slices;
}

double settlement_tick(double price) {
  if (price < 10.0) return 0.1;
  if (price < 50.0) return 0.5;
  if (price < 500.0) return 1.0;
  if (price < 1000.0) return 5.0;
  return 10.0;
}

ButterflyCounts count_butterflies(std::span<const StrikePrice> points) {
  ButterflyCounts counts;
  for (const taifex::OptionRight right : {taifex::OptionRight::kCall, taifex::OptionRight::kPut}) {
    std::vector<StrikePrice> side;
    std::ranges::copy_if(points, std::back_inserter(side),
                         [right](const StrikePrice& p) { return p.right == right; });
    std::ranges::stable_sort(side, {}, &StrikePrice::strike);
    for (std::size_t i = 2; i < side.size(); ++i) {
      const StrikePrice& p1 = side[i - 2];
      const StrikePrice& p2 = side[i - 1];
      const StrikePrice& p3 = side[i];
      // Operation order as in Phase 1's script, so a value at the guard rounds the same way.
      const double w = (p3.strike - p2.strike) / (p3.strike - p1.strike);
      const double excess = p2.price - ((w * p1.price) + ((1.0 - w) * p3.price));
      const double band = (w * settlement_tick(p1.price) / 2.0) +
                          (settlement_tick(p2.price) / 2.0) +
                          ((1.0 - w) * settlement_tick(p3.price) / 2.0);
      ++counts.triples;
      counts.non_convex += excess > kButterflyGuard ? 1 : 0;
      counts.beyond_half_tick += excess > band + kButterflyGuard ? 1 : 0;
    }
  }
  return counts;
}

ButterflyCounts count_input_butterflies(const CalibrationSlice& slice) {
  std::vector<StrikePrice> points;
  points.reserve(slice.rows.size());
  for (const taifex::OptionRow& row : slice.rows) {
    points.push_back(
        {.strike = row.strike, .price = row.settlement_price.value_or(0.0), .right = row.right});
  }
  return count_butterflies(points);
}

}  // namespace aadp
