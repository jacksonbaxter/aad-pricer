#ifndef AADP_DATA_FORWARDS_HPP_
#define AADP_DATA_FORWARDS_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"

// Forward, discount factor and time to expiry for each TXO expiry, as docs/NOTES.md, "Stage 6b",
// defines them, after Phase 1's chain profile. Everything here is computed from settlements,
// which are not tradable prices.
namespace aadp {

// One strike's settlement C - P.
struct ParityPoint {
  double strike = 0.0;
  double call_minus_put = 0.0;
};

// Phase 1's settle_all: every strike of the expiry with both a call and a put row of contract TXO
// in the regular session, both settled, in increasing strike. If a strike had two rows for one
// right, the later one would count, as in Phase 1's script; the capture has none.
std::vector<ParityPoint> settlement_parity_points(std::span<const taifex::OptionRow> rows,
                                                  std::string_view expiry);

// Ordinary least squares of C - P = D (F - K) on K: D = -slope, F = intercept / D.
struct ParityFit {
  std::size_t points = 0;
  double forward = 0.0;
  double discount = 0.0;
  // Standard errors from the residual variance over n - 2; the forward's by the delta method.
  double forward_se = 0.0;
  double discount_se = 0.0;
  // sqrt(sum of squared residuals / n), divided by n as Phase 1's script does.
  double rms_residual = 0.0;
  // rms_residual / sqrt(2): the noise of one leg, if the call and the put carry independent
  // noise of the same size.
  double sigma_leg = 0.0;
  double max_abs_residual = 0.0;
};

enum class ParityError : std::uint8_t {
  kTooFewPoints,  // Fewer than 3.
  kNonFinite,     // A strike or a difference is not finite.
  kNoStrikeSpread,
  kNonPositiveDiscount,  // The slope is not negative.
};

std::expected<ParityFit, ParityError> fit_parity(std::span<const ParityPoint> points);

// The TX futures settlement of the regular session for an expiry, the cross-check on F. Weeklies
// have no TX month.
std::optional<double> tx_settlement(std::span<const taifex::FuturesRow> rows,
                                    std::string_view expiry);

// The trade date of the captures, and the times that define T. TAIFEX's regular session closes
// at 13:45 Taipei, and on the last trading day at 13:30 (Rules Art. 8). Taipei keeps no daylight
// saving, so local clock times subtract directly.
inline constexpr std::int32_t kSnapshotTradeDate = 20241115;
inline constexpr int kSnapshotMinutes = (13 * 60) + 45;
inline constexpr int kExpiryMinutes = (13 * 60) + 30;
inline constexpr double kSecondsPerYear = 365.0 * 86400.0;  // ACT/365, in seconds.

// The last trading day of each TXO expiry listed on the trade date, YYYYMMDD, from TAIFEX's final
// settlement table, as Phase 1 confirmed them.
std::optional<std::int32_t> txo_expiry_date(std::string_view expiry);

// Seconds from 13:45 on the trade date to 13:30 on the expiry date. Empty for an unknown expiry.
std::optional<std::int64_t> txo_seconds_to_expiry(std::string_view expiry);

// The same over kSecondsPerYear.
std::optional<double> txo_years_to_expiry(std::string_view expiry);

}  // namespace aadp

#endif  // AADP_DATA_FORWARDS_HPP_
