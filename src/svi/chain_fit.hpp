// The TAIFEX chain into the raw SVI fit: parity forward, discount and sigma_leg per expiry, time to
// expiry, the calibration set, the noise weights, the fit, and the arbitrage checks on the fitted
// slices, as NOTES.md, "Stage 6b", pre-registers them. The inputs are settlements, not tradable
// quotes, and the usable surface reaches 33 days only.
#ifndef AADP_SVI_CHAIN_FIT_HPP_
#define AADP_SVI_CHAIN_FIT_HPP_

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "data/calibration_set.hpp"
#include "data/forwards.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
#include "svi/arbitrage.hpp"
#include "svi/fit.hpp"

namespace aadp::svi {

// NOTES.md, "Recommended calibration set": the three expiries with market-anchored settlements.
inline constexpr std::array<std::string_view, 3> kCalibrationExpiries = {"202411", "202411W4",
                                                                         "202412"};

struct ChainSlice {
  std::string expiry;
  double years = 0.0;  // T, ACT/365 in seconds.
  ParityFit parity;    // F, D and sigma_leg from settle_all.
  std::optional<double> tx_settlement;
  CalibrationSlice rows;
  ButterflyCounts input_butterflies;
  // The quotes in the order of rows, noise_i^2 = sigma_leg^2 + tick_i^2 / 12.
  FitSlice quotes;
};

enum class ChainErrorCode : std::uint8_t {
  kWrongTradeDate,  // A TXO row of the expiry is not from the snapshot's trade date.
  kUnknownExpiry,   // No expiry date is known for it, so T is undefined.
  kParity,          // The regression failed; parity_error says how.
  kCalibrationSet,
};

struct ChainError {
  ChainErrorCode code = ChainErrorCode::kParity;
  std::string expiry;
  ParityError parity_error = ParityError::kTooFewPoints;
};

[[nodiscard]] const char* ChainErrorName(ChainErrorCode code) noexcept;

// One ChainSlice per expiry, in the order given.
[[nodiscard]] std::expected<std::vector<ChainSlice>, ChainError> BuildChainSlices(
    std::span<const taifex::OptionRow> options, std::span<const taifex::FuturesRow> futures,
    std::span<const std::string_view> expiries);

struct SliceFit {
  std::expected<FitResult, FitError> fit = std::unexpected(FitError{});
  double svi6_bp = 0.0;               // SVI-6, unweighted.
  double svi11_vol_points = 0.0;      // SVI-11, unweighted.
  ButterflyReport butterfly;          // stage 6a's checks on the fitted parameters, as fitted
  ButterflyCounts model_butterflies;  // Phase 1's triple count on the model prices
};

struct CalendarPair {
  std::size_t earlier = 0;
  std::size_t later = 0;
  CalendarReport report;
};

struct SurfaceFit {
  std::vector<SliceFit> slices;
  // Between each pair of adjacent slices that both fitted.
  std::vector<CalendarPair> calendar;
};

// Fits each slice on its own, with no calendar constraint, then checks: nothing is repaired.
[[nodiscard]] SurfaceFit FitChain(std::span<const ChainSlice> slices,
                                  const FitOptions& options = {});

}  // namespace aadp::svi

#endif  // AADP_SVI_CHAIN_FIT_HPP_
