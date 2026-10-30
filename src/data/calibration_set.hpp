#ifndef AADP_DATA_CALIBRATION_SET_HPP_
#define AADP_DATA_CALIBRATION_SET_HPP_

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "data/taifex_options.hpp"

// The row filter from NOTES.md, "Recommended calibration set". This selects rows and fits nothing.
namespace aadp {

inline constexpr std::string_view kCalibrationContract = "TXO";
// A project choice, not a TAIFEX rule: half a tick is then at most 5% of the price.
inline constexpr double kMinSettlementPoints = 1.0;

// Forwards come from the caller, because the parity regression that produces them is Phase 3.
struct ExpiryForward {
  std::string expiry;
  double forward;
};

// Settlements, not tradable prices.
struct CalibrationSlice {
  std::string expiry;
  double forward = 0.0;
  std::vector<taifex::OptionRow> rows;
};

enum class CalibrationError : std::uint8_t {
  kBadForward,  // Not finite, or not positive.
  kDuplicateExpiry,
};

// One slice per entry of `forwards`, in that order, holding the rows that are all of:
// - contract TXO, on that expiry, in the regular session, never after-hours
// - out of the money by that forward: a put below it or a call above it
// - volume > 0
// - settlement_price == last_trade_price
// - settlement_price >= kMinSettlementPoints
// Which expiries to use is the caller's choice: the set in NOTES.md is 202411, 202411W4, 202412.
std::expected<std::vector<CalibrationSlice>, CalibrationError> select_calibration_set(
    std::span<const taifex::OptionRow> rows, std::span<const ExpiryForward> forwards);

}  // namespace aadp

#endif  // AADP_DATA_CALIBRATION_SET_HPP_
