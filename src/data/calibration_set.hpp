#ifndef AADP_DATA_CALIBRATION_SET_HPP_
#define AADP_DATA_CALIBRATION_SET_HPP_

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "data/taifex_options.hpp"

// The row filter from docs/NOTES.md, "Recommended calibration set". This selects rows and fits
// nothing.
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
// Which expiries to use is the caller's choice: the set in docs/NOTES.md is 202411, 202411W4,
// 202412.
std::expected<std::vector<CalibrationSlice>, CalibrationError> select_calibration_set(
    std::span<const taifex::OptionRow> rows, std::span<const ExpiryForward> forwards);

// The minimum price fluctuation at a premium level, TAIFEX Rules Art. 7: 0.1 below 10 points,
// 0.5 below 50, 1 below 500, 5 below 1,000 and 10 above.
double settlement_tick(double price);

// One price at one strike, for the butterfly count.
struct StrikePrice {
  double strike = 0.0;
  double price = 0.0;
  taifex::OptionRight right = taifex::OptionRight::kCall;
};

struct ButterflyCounts {
  std::size_t triples = 0;
  std::size_t non_convex = 0;
  std::size_t beyond_half_tick = 0;
};

// Phase 1's input butterfly count (chain-verify/scripts/profile_recommended.py), stated in
// docs/NOTES.md, "Stage 6b". For each right, strikes in increasing order, every three adjacent
// points: with w = (K3 - K2) / (K3 - K1), the excess is P2 - (w P1 + (1 - w) P3). Non-convex:
// excess above kButterflyGuard. Beyond half a tick: above w h1 + h2 + (1 - w) h3 + kButterflyGuard,
// h being half of settlement_tick at each price. The guard keeps a difference that is only decimal
// rounding from counting; the smallest half-tick is 0.05.
inline constexpr double kButterflyGuard = 1e-9;
ButterflyCounts count_butterflies(std::span<const StrikePrice> points);

// The same on a slice's settlements.
ButterflyCounts count_input_butterflies(const CalibrationSlice& slice);

}  // namespace aadp

#endif  // AADP_DATA_CALIBRATION_SET_HPP_
