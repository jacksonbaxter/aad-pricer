// The three contracts stage 9 pre-registered (docs/NOTES.md, "Stage 9"): the terms of the GBM
// workloads in bench/aad_workloads.cpp, restated here so that the tests and the report do not
// reach into bench/.
#ifndef AADP_F32_CONTRACTS_HPP_
#define AADP_F32_CONTRACTS_HPP_

#include <array>
#include <cstdint>
#include <string_view>

#include "pricer/contract.hpp"

namespace aadp::f32 {

struct Workload {
  std::string_view name;
  pricer::Contract contract;
  pricer::Market<double> market;
};

// Arithmetic Asian call, S0 = K = 100, sigma 0.2, r 0.03, q 0, T 1: aad4-asian12 and mc-asian252.
constexpr pricer::Contract AsianCall(std::uint32_t fixings) {
  pricer::Contract c;
  c.product = pricer::Product::kAsianArithmetic;
  c.type = pricer::OptionType::kCall;
  c.strike = 100.0;
  c.steps = fixings;
  return c;
}

// Up-and-out call, S0 100, K 120, H 150, discrete at 156 dates, one-step survival, T 3: the
// contract of aad8-barrier.
constexpr pricer::Contract OneStepBarrier() {
  pricer::Contract c;
  c.product = pricer::Product::kBarrier;
  c.type = pricer::OptionType::kCall;
  c.strike = 120.0;
  c.steps = 156;
  c.barrier = {.direction = pricer::Direction::kUp,
               .knock = pricer::Knock::kOut,
               .monitoring = pricer::Monitoring::kDiscrete,
               .treatment = pricer::Treatment::kOneStep,
               .level = 150.0,
               .smoothing_width = 0.0};
  return c;
}

inline constexpr pricer::Market<double> kAsianMarket{
    .spot = 100.0, .sigma = 0.2, .r = 0.03, .q = 0.0, .maturity = 1.0};
inline constexpr pricer::Market<double> kBarrierMarket{
    .spot = 100.0, .sigma = 0.2, .r = 0.03, .q = 0.0, .maturity = 3.0};

inline constexpr std::array<Workload, 3> kWorkloads = {{
    {.name = "asian12", .contract = AsianCall(12), .market = kAsianMarket},
    {.name = "asian252", .contract = AsianCall(252), .market = kAsianMarket},
    {.name = "barrier156", .contract = OneStepBarrier(), .market = kBarrierMarket},
}};

// The pre-registered seeds and sizes. Development seeds only: never 1729 or 271828.
inline constexpr std::uint64_t kTestSeed = 20261201;
inline constexpr std::uint64_t kTableSeed = 20261202;
inline constexpr std::uint64_t kTimingSeed = 20261203;
inline constexpr std::uint64_t kPropertySeed = 20261204;
inline constexpr std::uint32_t kSpan = 16;
inline constexpr double kGpu2Factor = 0.01;

}  // namespace aadp::f32

#endif  // AADP_F32_CONTRACTS_HPP_
