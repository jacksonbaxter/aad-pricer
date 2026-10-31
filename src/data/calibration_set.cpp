#include "data/calibration_set.hpp"

#include <algorithm>
#include <cmath>
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

}  // namespace aadp
