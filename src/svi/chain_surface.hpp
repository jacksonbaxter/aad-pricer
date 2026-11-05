// The fitted TAIFEX chain as a pricing surface, and the three svi-surface contracts that NOTES.md,
// "Stage 7a", pre-registers on it. Shared by the real-data test and aadp_local_vol_report, so both
// price exactly the same thing.
#ifndef AADP_SVI_CHAIN_SURFACE_HPP_
#define AADP_SVI_CHAIN_SURFACE_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "pricer/contract.hpp"
#include "pricer/local_vol.hpp"
#include "svi/chain_fit.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::svi {

struct ChainSurface {
  std::vector<SliceTerms> terms;
  std::vector<RawSvi<double>> params;
};

// Each slice's T, parity F and D, and fitted parameters, as fitted. Empty if any fit failed.
inline std::optional<ChainSurface> SurfaceFromChain(std::span<const ChainSlice> slices,
                                                    const SurfaceFit& fit) {
  if (fit.slices.size() != slices.size()) return std::nullopt;
  ChainSurface out;
  for (std::size_t j = 0; j < slices.size(); ++j) {
    if (!fit.slices[j].fit) return std::nullopt;
    out.terms.push_back({.expiry = slices[j].years,
                         .forward = slices[j].parity.forward,
                         .discount = slices[j].parity.discount});
    out.params.push_back(fit.slices[j].fit->params);
  }
  return out;
}

// The contract terms: K = 22,800, H = 21,500, 33 dates, maturing at the last slice, n = 4.
inline constexpr double kSurfaceStrike = 22800.0;
inline constexpr double kSurfaceBarrier = 21500.0;
inline constexpr std::uint32_t kSurfaceDates = 33;
inline constexpr std::uint32_t kSurfaceSubsteps = 4;

struct NamedContract {
  const char* name = "";
  pricer::LocalVolContract contract;
};

inline pricer::Contract SurfaceDownAndOut(pricer::Monitoring monitoring,
                                          pricer::Treatment treatment) {
  pricer::Contract c;
  c.product = pricer::Product::kBarrier;
  c.strike = kSurfaceStrike;
  c.steps = kSurfaceDates;
  c.barrier.direction = pricer::Direction::kDown;
  c.barrier.knock = pricer::Knock::kOut;
  c.barrier.monitoring = monitoring;
  c.barrier.treatment = treatment;
  c.barrier.level = kSurfaceBarrier;
  return c;
}

// The arithmetic Asian call, the discrete down-and-out call by one-step survival and the
// continuous one by the bridge, maturing at `maturity`, the last slice's expiry.
inline std::array<NamedContract, 3> SurfaceContracts(double maturity) {
  pricer::Contract asian;
  asian.product = pricer::Product::kAsianArithmetic;
  asian.strike = kSurfaceStrike;
  asian.steps = kSurfaceDates;
  const auto lv = [&](const pricer::Contract& c) {
    return pricer::LocalVolContract{
        .contract = c, .maturity = maturity, .substeps = kSurfaceSubsteps};
  };
  return {{
      {.name = "asian-arithmetic-call", .contract = lv(asian)},
      {.name = "down-out-call-discrete-one-step",
       .contract =
           lv(SurfaceDownAndOut(pricer::Monitoring::kDiscrete, pricer::Treatment::kOneStep))},
      {.name = "down-out-call-continuous-bridge",
       .contract =
           lv(SurfaceDownAndOut(pricer::Monitoring::kContinuous, pricer::Treatment::kBridge))},
  }};
}

}  // namespace aadp::svi

#endif  // AADP_SVI_CHAIN_SURFACE_HPP_
