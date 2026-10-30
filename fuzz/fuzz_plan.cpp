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
    if (row.status != aadp::RowStatus::kRun) continue;
    Require(row.paths > 0 && row.paths % row.batches == 0);
    Require(std::isfinite(row.se_ceiling) && row.se_ceiling > 0.0);
    Require(std::isfinite(row.b) && row.b >= 0.0);
    const auto instrument = aadp::DecodeInstrument(row.instrument);
    Require(instrument.has_value());
    Require(instrument->market.spot > 0.0 && instrument->contract.steps >= 1);
    PriceOnePath(*instrument);
    const auto greek = aadp::DecodeGreek(row.greek);
    Require(greek.has_value());
    const auto step = aadp::FiniteDifferenceStep(row.reference);
    if (step) Require(*step > 0.0 && *greek != aadp::Greek::kValue);
  }
  const std::string encoded = aadp::EncodePlan(*plan);
  const auto again = aadp::DecodePlan(encoded);
  Require(again.has_value());
  Require(*again == *plan);
  Require(aadp::EncodePlan(*again) == encoded);
  return 0;
}
