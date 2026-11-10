#include "svi/chain_essvi.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "data/calibration_set.hpp"
#include "svi/arbitrage.hpp"
#include "svi/chain_fit.hpp"
#include "svi/chain_surface.hpp"
#include "svi/essvi_fit.hpp"
#include "svi/fit.hpp"

namespace aadp::svi {

EssviChainFit FitChainEssvi(std::span<const ChainSlice> slices, const EssviFitOptions& options) {
  EssviChainFit out;
  std::vector<FitSlice> quotes;
  quotes.reserve(slices.size());
  for (const ChainSlice& s : slices) quotes.push_back(s.quotes);
  out.fit = FitEssviSurface(quotes, options);
  if (!out.fit) return out;
  const EssviFitResult& fit = *out.fit;
  for (std::size_t j = 0; j < slices.size(); ++j) {
    const EssviSliceFit& f = fit.slices[j];
    EssviChainSlice s;
    s.svi6_bp = MeanAbsPriceErrorBp(slices[j].quotes, f.model_price);
    s.svi11_vol_points = ImpliedVolRmse(f.model_vol, f.market_vol);
    s.butterfly = CheckButterfly(f.raw);
    std::vector<StrikePrice> model;
    model.reserve(f.model_price.size());
    for (std::size_t i = 0; i < f.model_price.size(); ++i) {
      model.push_back({.strike = slices[j].rows.rows[i].strike,
                       .price = f.model_price[i],
                       .right = slices[j].rows.rows[i].right});
    }
    s.model_butterflies = count_butterflies(model);
    out.slices.push_back(std::move(s));
  }
  for (std::size_t j = 1; j < slices.size(); ++j) {
    out.calendar.push_back({.earlier = j - 1,
                            .later = j,
                            .report = CheckCalendar(fit.slices[j - 1].raw, fit.slices[j].raw)});
  }
  return out;
}

std::optional<ChainSurface> SurfaceFromEssvi(std::span<const ChainSlice> slices,
                                             const EssviChainFit& fit) {
  if (!fit.fit || fit.fit->slices.size() != slices.size()) return std::nullopt;
  ChainSurface out;
  for (std::size_t j = 0; j < slices.size(); ++j) {
    out.terms.push_back({.expiry = slices[j].years,
                         .forward = slices[j].parity.forward,
                         .discount = slices[j].parity.discount});
    out.params.push_back(fit.fit->slices[j].raw);
  }
  return out;
}

}  // namespace aadp::svi
