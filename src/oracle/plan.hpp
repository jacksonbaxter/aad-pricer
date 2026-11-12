// Decoder for references/comparison_plan.csv, the pre-registered list of comparisons.
//
// Comment lines start with '#'. Lines starting "#! key=value" are directives, and K, the critical
// value and the seeds live there. Columns:
//   id,instrument,greek,reference,N,B,seed,se_ceiling,b,status
// A run row names a contract, a Greek, and a reference id or "FD:<h>". The contract is one of
//   a GBM kind, "<kind>:<key>=<value>;..."     the frozen kernels' and the pricer's contracts
//   knock-in-parity:discrete|continuous        catalog.hpp: a knock-in on a catalog contract
//   svi-surface:<product>                      catalog.hpp: a contract on the fitted chain, whose
//                                              Greek is d_<p>_<dNN> and reference "FD:<h>"
// An unresolved row is a surface row whose finite-difference ladder never settled (docs/NOTES.md,
// "Stage 8"): it has N, seed and "FD:<h>", writes "-" for se_ceiling and b, is run and is never
// passed. An awaiting-phase3 row counts toward K and writes "-" for what cannot be fixed until
// the code exists: N, seed, se_ceiling and b.
#ifndef AADP_ORACLE_PLAN_HPP_
#define AADP_ORACLE_PLAN_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mc/gbm_kernels.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/decode_util.hpp"
#include "oracle/finite_difference.hpp"

namespace aadp {

enum class RowStatus : std::uint8_t { kRun, kAwaitingPhase3, kUnresolved };

struct Instrument {
  Contract contract;
  MarketParams<double> market{};
  // A knock-in by parity on the knock-out contract above. The frozen kernels have none, so only
  // the production estimator prices it.
  bool knock_in = false;
};

enum class InstrumentKind : std::uint8_t { kGbm, kKnockIn, kSurface };

struct RowInstrument {
  InstrumentKind kind = InstrumentKind::kGbm;
  Instrument gbm;               // kGbm and kKnockIn
  std::string_view parameters;  // What a reference row must quote; kGbm and kKnockIn
  std::size_t surface = 0;      // kSurface: index into catalog::kSurfaceInstruments
};

// A surface row's Greek: the derivative in parameter `parameter` (catalog::kSurfaceParameters)
// of slice `slice` (catalog::kSurfaceSlices), input 5 slice + parameter of the surface gradient.
struct SurfaceGreek {
  std::size_t slice = 0;
  std::size_t parameter = 0;
  [[nodiscard]] std::size_t Index() const { return (5 * slice) + parameter; }
};

struct PlanRow {
  std::string id;
  std::string instrument;  // Verbatim. Run rows also decode it below.
  std::string greek;
  std::string reference;
  std::uint64_t paths = 0;  // Zero on an awaiting row.
  std::uint32_t batches = 0;
  std::uint64_t seed = 0;
  double se_ceiling = 0.0;
  double b = 0.0;
  RowStatus status = RowStatus::kRun;

  bool operator==(const PlanRow&) const = default;
};

struct PlanHeader {
  std::uint64_t declared_k = 0;
  double alpha_f = 0.0;
  std::uint32_t batches = 0;
  double critical = 0.0;
  double ceiling_factor = 0.0;
  double fd_ceiling_factor = 0.0;
  double fd_b_factor = 0.0;
  std::uint64_t pilot_seed = 0;
  std::uint64_t measured_seed = 0;

  bool operator==(const PlanHeader&) const = default;
};

struct Plan {
  PlanHeader header;
  std::vector<std::string> comments;
  std::vector<PlanRow> rows;

  bool operator==(const Plan&) const = default;
};

// Returns the error rather than aborting on anything: this reads a file.
std::expected<Plan, decode::DecodeError> DecodePlan(std::string_view input);
std::string EncodePlan(const Plan& plan);

// "<kind>:<key>=<value>;...", every key exactly once.
std::optional<Instrument> DecodeInstrument(std::string_view text);
// The part after the kind, which a reference row must quote exactly.
std::string_view InstrumentParameters(std::string_view text);

// Any instrument a run row may name, through the catalog's tables for the two aliases. The
// returned parameters view the catalog's text or `text` itself.
std::optional<RowInstrument> ResolveInstrument(std::string_view text);

std::optional<SurfaceGreek> DecodeSurfaceGreek(std::string_view text);
std::optional<Greek> DecodeGreek(std::string_view text);
const char* GreekName(Greek greek);

// h from "FD:<h>", or nothing if the reference is an id.
std::optional<double> FiniteDifferenceStep(std::string_view reference);

}  // namespace aadp

#endif  // AADP_ORACLE_PLAN_HPP_
