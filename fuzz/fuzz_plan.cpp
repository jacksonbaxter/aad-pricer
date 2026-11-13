// Plan decoder. Properties: no crash on any bytes, an error always says why, and an accepted plan
// meets every invariant the runner relies on and survives a round trip through its encoder.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "mc/gbm_kernels.hpp"
#include "oracle/catalog.hpp"
#include "oracle/finite_difference.hpp"
#include "oracle/plan.hpp"

namespace {

void Require(bool condition) {
  if (!condition) std::abort();
}

// Bounded, because m may legally be large.
void PriceOnePath(const aadp::Instrument& instrument) {
  if (instrument.contract.steps > 64) return;
  std::vector<double> uniforms(instrument.contract.steps, 0.3);
  (void)aadp::DiscountedPayoff<double>(instrument.contract, instrument.market, uniforms);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const auto plan = aadp::DecodePlan(input);
  if (!plan) {
    Require(!plan.error().message.empty());
    return 0;
  }
  Require(plan->header.batches >= 2);
  std::set<std::string> ids;
  for (const aadp::PlanRow& row : plan->rows) {
    Require(ids.insert(row.id).second);
    Require(row.batches == plan->header.batches);
    if (row.status == aadp::RowStatus::kAwaitingPhase3) continue;
    Require(row.paths > 0 && row.paths % row.batches == 0);
    // The runner resolves a row's instrument as the decoder did: a GBM contract, a knock-in by
    // parity or a surface contract (stage 8), not a GBM contract alone.
    const auto instrument = aadp::ResolveInstrument(row.instrument);
    if (!instrument) __builtin_trap();
    const auto step = aadp::FiniteDifferenceStep(row.reference);
    if (instrument->kind == aadp::InstrumentKind::kSurface) {
      Require(instrument->surface < aadp::catalog::kSurfaceInstruments.size());
      Require(aadp::DecodeSurfaceGreek(row.greek).has_value());
      Require(step.has_value() && *step > 0.0);
    } else {
      Require(row.status == aadp::RowStatus::kRun);
    }
    if (row.status != aadp::RowStatus::kRun) continue;
    Require(std::isfinite(row.se_ceiling) && row.se_ceiling > 0.0);
    Require(std::isfinite(row.b) && row.b >= 0.0);
    if (instrument->kind == aadp::InstrumentKind::kSurface) continue;
    const aadp::Instrument& gbm = instrument->gbm;
    Require(gbm.knock_in == (instrument->kind == aadp::InstrumentKind::kKnockIn));
    Require(gbm.market.spot > 0.0 && gbm.contract.steps >= 1);
    PriceOnePath(gbm);
    const auto greek = aadp::DecodeGreek(row.greek);
    if (!greek) __builtin_trap();
    if (step) Require(*step > 0.0 && *greek != aadp::Greek::kValue);
  }
  const std::string encoded = aadp::EncodePlan(*plan);
  const auto again = aadp::DecodePlan(encoded);
  Require(again.has_value());
  Require(*again == *plan);
  Require(aadp::EncodePlan(*again) == encoded);
  return 0;
}
