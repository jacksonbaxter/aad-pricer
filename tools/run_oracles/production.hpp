// The production estimator of measured run 2 and stage 8's pilot (docs/NOTES.md, "Stage 8").
//
// Every row's estimate is a batch mean of the production code: ad::PriceWithGradient for the 102
// GBM rows (the value and the five tape Greeks, theta = -dV/dT), and ad::PriceLocalVolWithGradient
// for the 45 surface rows (component 5 j + i for slice j and parameter i). A finite-difference
// reference is a central difference of the production price, pricer::Price or
// pricer::PriceLocalVol, on the same uniforms, batch by batch. The frozen kernels and the Dual
// play no part here.
//
// The surface rows price stage 7a's three contracts on the raw SVI fit of the captured chain
// (svi/chain_surface.hpp): TAIFEX settlements of one day, 2024-11-15, not tradable quotes,
// reaching 33 days.
#ifndef AADP_TOOLS_RUN_ORACLES_PRODUCTION_HPP_
#define AADP_TOOLS_RUN_ORACLES_PRODUCTION_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "ladder.hpp"
#include "oracle/finite_difference.hpp"
#include "oracle/plan.hpp"
#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/runner.hpp"
#include "runner.hpp"
#include "svi/chain_surface.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::oracle_runner {

// The fitted chain under data_dir, as stage 7a fits it and prices on it, or why not.
std::expected<svi::ChainSurface, std::string> LoadChainSurface(const std::string& data_dir);

// Stage 7a's contract for catalog::kSurfaceInstruments[index], maturing at the last slice.
pricer::LocalVolContract SurfaceContract(std::size_t index, const svi::ChainSurface& surface);

// The batch means of a GBM row's value and five tape Greeks, in Greek order (value, delta, vega,
// rho, dividend rho, theta), from ad::PriceWithGradient. A knock-in instrument prices with
// Knock::kIn, which the pricer takes as vanilla minus the knock-out on the same path.
using GbmBatches = std::array<std::vector<double>, 1 + kFirstOrderGreeks>;
std::expected<GbmBatches, std::string> GbmTapeBatches(const Instrument& instrument,
                                                      const pricer::RunConfig& config);

struct ProductionOptions {
  std::uint32_t threads = 1;
  bool smoke = false;  // As RunOptions: N = 64 B, a seed apart from both reserved ones.
  std::uint64_t smoke_paths_per_batch = 64;
  // The fingerprint's development run: this seed and N / paths_divisor in place of the plan's.
  std::uint64_t seed_override = 0;
  std::uint64_t paths_divisor = 1;
};

// The floor tally of one bumped surface price against the base run on the same paths.
struct TallyChange {
  std::int64_t butterfly_plus = 0;
  std::int64_t butterfly_minus = 0;
  std::int64_t capped_plus = 0;
  std::int64_t capped_minus = 0;
};

struct ProductionRow {
  std::string id;
  std::string verdict;  // pass, FAIL, UNRESOLVED, finite, NONFINITE, or SKIPPED
  std::string problem;  // why a row was skipped
  ToleranceResult result;
  bool surface = false;
  pricer::FloorTally base;  // surface rows: the tape run's tally
  TallyChange tally;
};

// All rows of the plan; a plan with an awaiting-phase3 row is refused. Without a surface the 45
// surface rows are SKIPPED: no data, which fails the run as any skip does. Writes the runner's
// row lines, a tally line under each surface row, and the summary. rows_out, if given, receives
// every row's numbers in plan order.
RunSummary RunPlanProduction(const Plan& plan, const ReferenceTable& references,
                             const svi::ChainSurface* surface, const ProductionOptions& options,
                             std::ostream& out, std::vector<ProductionRow>* rows_out = nullptr);

// One central difference of PriceLocalVol in input 5 j + i, with its tallies against `base`.
struct SurfaceDifference {
  std::vector<double> batches;  // (V_q(theta + h) - V_q(theta - h)) / 2h
  TallyChange tally;
};
std::expected<SurfaceDifference, std::string> SurfaceCentralDifference(
    const pricer::LocalVolContract& contract, std::span<const svi::SliceTerms> terms,
    std::span<const svi::RawSvi<double>> params, std::size_t input, double h,
    const pricer::RunConfig& config, const pricer::FloorTally& base);

// One surface row's ladder on the pilot's paths.
struct SurfaceLadder {
  std::size_t input = 0;
  double h0 = 0.0;
  std::string refused;  // why the step left the domain; empty if it did not
  LadderOutcome outcome;
  std::array<TallyChange, kLadderLevels> tallies{};
  std::vector<double> d_star;  // D at h*, or at h_8 when unresolved
};

struct SurfacePilot {
  pricer::Estimate value;
  std::vector<pricer::Estimate> gradient;  // the tape, 5 per slice
  pricer::FloorTally base;
  std::vector<SurfaceLadder> rows;  // in the order of `inputs`
};

// The tape run and the full ladder of each input, on one contract and one set of paths.
std::expected<SurfacePilot, std::string> PilotSurface(const pricer::LocalVolContract& contract,
                                                      std::span<const svi::SliceTerms> terms,
                                                      std::span<const svi::RawSvi<double>> params,
                                                      const pricer::RunConfig& config,
                                                      double critical,
                                                      std::span<const std::size_t> inputs);

struct Stage8PilotConfig {
  PilotConfig pilot;
  std::uint64_t second_seed = 0;  // Disclosure only: the SE at N_p and h* on another seed.
};

struct Stage8Pilot {
  std::vector<PilotRow> rows;
  std::vector<std::pair<std::string, SurfaceLadder>> ladders;
};

// The 57 rows of stage 8 at N / divisor. critical is the plan's. Logs every row's disclosure.
Stage8Pilot RunStage8Pilot(const std::vector<PlanTemplateRow>& rows,
                           const ReferenceTable& references, const svi::ChainSurface& surface,
                           const Stage8PilotConfig& config, double critical, std::ostream& log);

// references/svi_fd_ladder.csv: per row and level h, D, Delta, E, the verdict and the tally
// changes; per row the choice. DecodeLadderChoices reads it back.
std::string EncodeLadder(const std::vector<std::pair<std::string, SurfaceLadder>>& ladders);

// The development-seed fingerprint of run 2's code: every row's estimate, reference and SE as
// hex floats, at seed 20261128 and N / 1024. Surface rows only with a surface.
inline constexpr std::uint64_t kFingerprintSeed = 20261128;
inline constexpr std::uint64_t kFingerprintDivisor = 1024;
std::string Fingerprint(const Plan& plan, const ReferenceTable& references,
                        const svi::ChainSurface* surface, std::uint32_t threads);

}  // namespace aadp::oracle_runner

#endif  // AADP_TOOLS_RUN_ORACLES_PRODUCTION_HPP_
