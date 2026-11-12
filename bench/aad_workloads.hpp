// The AAD benchmark's workloads and the ways each is run (BENCHMARKS.md sections 1 to 3).
//
// Every variant of a workload prices the same contract on the same market, with the same seed,
// path count, batches, threads and kernels, in the same binary: common random numbers throughout.
//   price         Price(): value only, the scalar primal. The denominator of every multiple.
//   tape          PriceWithGradient(): value and five Greeks, the plain tape.
//   tape-skip     the same with the tape's zero-adjoint skip (EVIDENCE B9), timed apart.
//   tape-record   the tape recording every path and rewinding, with no sweep: the record part of
//                 tape, so reverse = tape - tape-record (EVIDENCE B31).
//   hand          the hand-written pathwise adjoint, where one exists (src/ad/hand_adjoint.hpp).
//   bump-central  2 N + 1 revaluations, N = 5 (src/pricer/bump.hpp), at the studied steps.
//   bump-forward  N + 1 revaluations.
//   price-lanes   simd::PriceLanes() at the build's widest lanes (src/simd): value only, Price()'s
//                 bits. The lane primal; the faster of price and price-lanes is the fastest primal.
//   tape-lanes    simd::PriceWithGradientLanes() at the same lanes: value and five Greeks on the
//                 vector tape, the tape's bits, with any group that fell back to the scalar tape
//                 counted (never in a pricer kernel).
// The lane variants run on the first-order pricer workloads, whose kernels are all branch-free on
// lanes (docs/NOTES.md, "Vector tape"). The second-order workloads and Lax-Wendroff have none: no
// lane form exists of the conditional kernels, forward over reverse, or the advection. The
// Lax-Wendroff worst case (lax_wendroff.hpp) has the same variants on its 100 inputs, so its bumps
// cost 201 and 101 primal runs.
//
// The second-order workloads (aad8-*) run price and tape, and these, on the conditional kernels
// of src/pricer/conditional_kernels.hpp (the last step's expectation in closed form):
//   cond-price    PriceConditional(): value only, the primal of the kernels below.
//   cond-tape     PriceWithConditionalGradient(): value and five Greeks, one reverse sweep per
//                 path. The first-order AAD the second-order variants are measured against.
//   for-spot      PriceWithHessian<1> on spot: value, five Greeks and the spot row of the Hessian
//                 (gamma, vanna, d2V/dS dr, dS dq, dS dT), one forward-over-reverse sweep per path.
//   for-spot-vol  PriceWithHessian<2> on spot and vol: the same and the vol row (volga), one
//                 sweep per path with two tangent lanes.
// No variant runs a second pass, and none of their multiples enters AAD-1 or AAD-4.
//
// The hand adjoint skips the reverse pass on a path out of the money; the plain tape does not.
// Both fairness answers are given: tape-skip is the tape with a skip, and every workload reports
// the fraction of paths whose payoff is not zero beside its multiples.
#ifndef AADP_BENCH_AAD_WORKLOADS_HPP_
#define AADP_BENCH_AAD_WORKLOADS_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ad/gradient.hpp"
#include "lax_wendroff.hpp"
#include "pricer/bump.hpp"
#include "pricer/contract.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::bench::aad {

// A development seed: never 1729 or 271828, which the oracle plan reserves.
inline constexpr std::uint64_t kSeed = 20261103;
// Batches for every SE. 50 divides both target path counts, 500,000 and 300,000.
inline constexpr std::uint32_t kMcBatches = 50;

enum class Kind : std::uint8_t { kPricer, kLaxWendroff };

enum class Variant : std::uint8_t {
  kPrice,
  kTape,
  kTapeSkip,
  kTapeRecord,
  kHand,
  kBumpCentral,
  kBumpForward,
  kCondPrice,
  kCondTape,
  kForSpot,
  kForSpotVol,
  kPriceLanes,
  kTapeLanes,
};
inline constexpr std::array<Variant, 13> kVariants = {
    Variant::kPrice,    Variant::kTape,        Variant::kTapeSkip,    Variant::kTapeRecord,
    Variant::kHand,     Variant::kBumpCentral, Variant::kBumpForward, Variant::kCondPrice,
    Variant::kCondTape, Variant::kForSpot,     Variant::kForSpotVol,  Variant::kPriceLanes,
    Variant::kTapeLanes};
std::string_view VariantName(Variant variant);
std::optional<Variant> ParseVariant(std::string_view name);

// A step chosen by the h study (bump_study.hpp), with the study's estimate of the bias it leaves,
// that estimate's SE, and the rule's bound on |bias|. Copied from bench/bump_size_study.csv,
// which a test checks.
struct StepChoice {
  double h = 0.0;
  double bias = 0.0;
  double bias_se = 0.0;
  double truncation = 0.0;
};
using StepChoices = std::array<StepChoice, pricer::kInputCount>;

struct Workload {
  std::string_view id;
  std::string_view targets;
  std::string_view description;
  std::string_view differences;  // From the target's workload, stated
  Kind kind = Kind::kPricer;
  pricer::Contract contract;
  pricer::Market<double> market{};
  std::uint64_t paths = 0;  // The target's path count. Lax-Wendroff: 1, one run.
  bool threaded = false;    // Runs on the --aadp-threads count, else on one thread.
  bool hand = false;
  // Runs price, tape and the conditional and second-order variants, and no others.
  bool second_order = false;
  std::string_view study;  // The workload whose h study this one uses: the same contract and N
  StepChoices central{};   // Lax-Wendroff: index 0 for every input
  StepChoices forward{};
};

std::span<const Workload> Workloads();
const Workload* FindWorkload(std::string_view id);
bool HasVariant(const Workload& workload, Variant variant);

// The run configuration of a workload at 1 / divisor of its path count. nullopt if that is not a
// whole number of paths per batch.
std::optional<pricer::RunConfig> ConfigFor(const Workload& workload, std::uint64_t divisor,
                                           std::uint32_t threads);

pricer::BumpSteps Steps(const Workload& workload, pricer::BumpScheme scheme);

// What one run of a variant produced. Pricer workloads fill value and greeks (batch means
// included); Lax-Wendroff fills value.mean, and gradient for the variants that differentiate.
struct VariantResult {
  Variant variant = Variant::kPrice;
  pricer::Estimate value;
  std::array<pricer::Estimate, pricer::kInputCount> greeks;
  bool has_greeks = false;
  lw::Field<double> gradient{};
  // for-spot and for-spot-vol: row k is the Hessian row of lane k's input (spot, then vol), raw
  // d2V / dx_k dx_i with no theta sign.
  std::vector<std::array<pricer::Estimate, pricer::kInputCount>> hessian_rows;
  // Bumps only: realised denominators and mean |V| (or |J|) at the up and down markets.
  std::vector<double> denominator;
  std::vector<double> absolute_up;
  std::vector<double> absolute_down;
  // tape-lanes only: groups, and paths in them, that fell back to the scalar tape.
  std::uint64_t fallback_groups = 0;
  std::uint64_t fallback_paths = 0;
};

// One run of a variant, as the timed loop runs it. Stateful (a Lax-Wendroff tape is kept and
// reused, as Hogan reuses memory), so call it on one thread. Each call returns the run's result.
using Runner = std::function<VariantResult()>;
Runner MakeRunner(const Workload& workload, Variant variant, const pricer::RunConfig& config);

// The tape's own report of one path (Lax-Wendroff: one run), after the set-up (EVIDENCE B3).
struct TapeShape {
  std::size_t setup_statements = 0;
  std::size_t statements_per_path = 0;  // Largest over the paths sampled
  std::size_t operands_per_path = 0;
  double mean_statements_per_path = 0.0;
  std::size_t bytes_per_path = 0;  // 1 + 12 k per statement (MEM-2), for the largest path
  std::size_t adjoint_bytes_per_path = 0;
  std::size_t allocated_bytes = 0;  // Every buffer one worker's tape holds at its peak
  std::size_t paths_sampled = 0;
};
TapeShape MeasureTape(const Workload& workload, const pricer::RunConfig& config);

// The vector tape's report at the lanes tape-lanes runs, in TapeShape's terms per path: a group's
// statement and operand bytes over its lanes (5 + 8 N per operand), for the largest group of the
// first block (tests/simd/lane_tape_memory_test.cpp counts the same).
struct LaneTapeShape {
  std::size_t lanes = 0;
  std::size_t statements_per_group = 0;  // Largest over the groups sampled
  std::size_t operands_per_group = 0;
  std::size_t operand_bytes = 0;  // Per operand
  double bytes_per_path = 0.0;    // Of the largest group, over its lanes
  std::size_t allocated_bytes = 0;
  std::size_t groups_sampled = 0;
};
// Precondition: HasVariant(workload, Variant::kTapeLanes).
LaneTapeShape MeasureLaneTape(const Workload& workload, const pricer::RunConfig& config);

// The lanes the lane variants run at: the build's widest (simd::MaxLanes), its Highway target, and
// the lane ceiling every lane speedup is read against, with the host's FP datapath where it halves
// the throughput of a 256-bit operation (platform::FpDatapathNote).
std::size_t LaneCount();
std::string LaneTarget();
std::string LaneCeiling();

// The fraction of paths whose discounted payoff is not zero: in the money and, for a barrier, not
// knocked out. The hand adjoint's reverse pass runs on these alone.
double NonzeroPayoffFraction(const Workload& workload, const pricer::RunConfig& config);

}  // namespace aadp::bench::aad

#endif  // AADP_BENCH_AAD_WORKLOADS_HPP_
