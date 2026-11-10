// The TAIFEX chain into the eSSVI surface fit, with stage 6a's checks on the converted slices and
// stage 6b's metrics, as NOTES.md, "Stage 7c", pre-registers them; and the converted surface as a
// pricing surface for 7a's local-volatility pricer. The inputs are settlements, not tradable
// quotes, and the surface reaches 33 days only.
#ifndef AADP_SVI_CHAIN_ESSVI_HPP_
#define AADP_SVI_CHAIN_ESSVI_HPP_

#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "data/calibration_set.hpp"
#include "svi/arbitrage.hpp"
#include "svi/chain_fit.hpp"
#include "svi/chain_surface.hpp"
#include "svi/essvi_fit.hpp"

namespace aadp::svi {

struct EssviChainSlice {
  double svi6_bp = 0.0;               // SVI-6, unweighted
  double svi11_vol_points = 0.0;      // SVI-11, unweighted
  ButterflyReport butterfly;          // stage 6a's checks on the converted slice
  ButterflyCounts model_butterflies;  // Phase 1's triple count on the model prices
};

struct EssviChainFit {
  std::expected<EssviFitResult, EssviFitError> fit = std::unexpected(EssviFitError{});
  std::vector<EssviChainSlice> slices;  // empty if the fit failed
  std::vector<CalendarPair> calendar;   // adjacent converted slices
};

// The slices must be in order of expiry, as kCalibrationExpiries are. Nothing is repaired.
[[nodiscard]] EssviChainFit FitChainEssvi(std::span<const ChainSlice> slices,
                                          const EssviFitOptions& options = {});

// Each slice's T, parity F and D, and its converted raw parameters. Empty if the fit failed.
[[nodiscard]] std::optional<ChainSurface> SurfaceFromEssvi(std::span<const ChainSlice> slices,
                                                           const EssviChainFit& fit);

}  // namespace aadp::svi

#endif  // AADP_SVI_CHAIN_ESSVI_HPP_
