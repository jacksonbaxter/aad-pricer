// Monte Carlo prices off a raw SVI surface under local volatility (NOTES.md, "Stage 7a").
//
//   ValidateLocalVol(contract, terms, params, rule)       kNone or why not
//   PriceLocalVol(contract, terms, params, config, rule)
//       -> {Estimate, FloorTally}, or the reason it refused
//
// The contract is the GBM pricer's Contract with its m dates (contract.steps), a maturity, which
// must not pass the surface's last expiry, and n Euler sub-steps per date. The surface is the
// slices' terms and raw parameters; the spot is D_0 F_0 (svi/surface.hpp). Uniforms, batches,
// blocks and threads are the GBM pricer's (runner.hpp), so the estimate and the tally are the same
// bits for any thread count.
#ifndef AADP_PRICER_LOCAL_VOL_HPP_
#define AADP_PRICER_LOCAL_VOL_HPP_

#include <cstdint>
#include <expected>
#include <span>

#include "pricer/contract.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::pricer {

struct LocalVolContract {
  Contract contract;  // contract.steps is the number of dates m
  double maturity = 0.0;
  std::uint32_t substeps = 1;  // n Euler steps per date
};

enum class LocalVolError : std::uint8_t {
  kNone,
  kSurface,   // surface says why
  kRule,      // floor and cap not 1e-8 <= floor < cap <= 100
  kProduct,   // not European, arithmetic Asian or a knock-out barrier, or include_spot
  kContract,  // Validate refused it; contract says why
  kMaturity,  // past the last slice: nothing is extrapolated
  kSubsteps,  // n < 1, or m n above kMaxSteps
  kRange,     // a path could leave the finite range at the cap's volatility
  kRun,       // run says why
};

struct LocalVolFailure {
  LocalVolError code = LocalVolError::kNone;
  svi::SurfaceError surface = svi::SurfaceError::kNone;
  ContractError contract = ContractError::kNone;
  RunError run = RunError::kNone;
};

[[nodiscard]] const char* LocalVolErrorName(LocalVolError error) noexcept;

[[nodiscard]] LocalVolFailure ValidateLocalVol(const LocalVolContract& lv,
                                               std::span<const svi::SliceTerms> terms,
                                               std::span<const svi::RawSvi<double>> params,
                                               const svi::LocalVolRule& rule);

struct LocalVolEstimate {
  Estimate value;
  FloorTally floors;
};

[[nodiscard]] std::expected<LocalVolEstimate, LocalVolFailure> PriceLocalVol(
    const LocalVolContract& lv, std::span<const svi::SliceTerms> terms,
    std::span<const svi::RawSvi<double>> params, const RunConfig& config,
    const svi::LocalVolRule& rule = {});

}  // namespace aadp::pricer

#endif  // AADP_PRICER_LOCAL_VOL_HPP_
