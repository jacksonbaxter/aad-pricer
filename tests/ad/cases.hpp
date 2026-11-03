// Every pricer product and every barrier treatment, for the tape checks.
#ifndef AADP_TESTS_AD_CASES_HPP_
#define AADP_TESTS_AD_CASES_HPP_

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pricer/contract.hpp"
#include "support/dual.hpp"

namespace aadp::ad::testing {

struct Case {
  std::string name;
  pricer::Contract contract;
};

inline constexpr pricer::Market<double> kMarket{
    .spot = 100.0, .sigma = 0.30, .r = 0.10, .q = 0.02, .maturity = 0.5};

inline pricer::Contract Plain(pricer::Product product, pricer::OptionType type, std::uint32_t steps,
                              bool include_spot = false) {
  pricer::Contract c;
  c.product = product;
  c.type = type;
  c.strike = 100.0;
  c.steps = steps;
  c.include_spot = include_spot;
  return c;
}

inline pricer::Contract WithBarrier(pricer::Direction direction, pricer::Knock knock,
                                    pricer::Monitoring monitoring, pricer::Treatment treatment,
                                    std::uint32_t steps) {
  using pricer::Direction;
  pricer::Contract c = Plain(
      pricer::Product::kBarrier,
      direction == Direction::kDown ? pricer::OptionType::kCall : pricer::OptionType::kPut, steps);
  c.barrier = {.direction = direction,
               .knock = knock,
               .monitoring = monitoring,
               .treatment = treatment,
               .level = direction == Direction::kDown ? 95.0 : 110.0,
               .smoothing_width = 0.01};
  return c;
}

inline const char* TreatmentName(pricer::Monitoring monitoring, pricer::Treatment treatment) {
  using enum pricer::Treatment;
  if (monitoring == pricer::Monitoring::kContinuous) {
    return treatment == kBridge ? "continuous bridge" : "continuous sampled crossing";
  }
  switch (treatment) {
    case kOneStep:
      return "discrete one-step";
    case kHard:
      return "discrete hard";
    case kSmoothed:
      return "discrete smoothed";
    case kBridge:
      break;
  }
  return "invalid";
}

// Every product, both option types, and every direction, knock and treatment of the barrier.
inline std::vector<Case> AllCases(std::uint32_t steps) {
  using enum pricer::Product;
  using enum pricer::OptionType;
  using enum pricer::Direction;
  using enum pricer::Knock;
  using enum pricer::Monitoring;
  using enum pricer::Treatment;
  std::vector<Case> cases = {
      {.name = "european call", .contract = Plain(kEuropean, kCall, steps)},
      {.name = "european put", .contract = Plain(kEuropean, kPut, steps)},
      {.name = "digital call", .contract = Plain(kDigital, kCall, steps)},
      {.name = "digital put", .contract = Plain(kDigital, kPut, steps)},
      {.name = "arithmetic Asian call with spot",
       .contract = Plain(kAsianArithmetic, kCall, steps, true)},
      {.name = "arithmetic Asian put", .contract = Plain(kAsianArithmetic, kPut, steps)},
      {.name = "geometric Asian call", .contract = Plain(kAsianGeometric, kCall, steps)},
      {.name = "geometric Asian put with spot",
       .contract = Plain(kAsianGeometric, kPut, steps, true)},
  };
  constexpr std::array<std::pair<pricer::Monitoring, pricer::Treatment>, 5> kTreatments = {
      std::pair{kContinuous, kBridge}, std::pair{kContinuous, kHard},
      std::pair{kDiscrete, kOneStep}, std::pair{kDiscrete, kHard}, std::pair{kDiscrete, kSmoothed}};
  for (const auto direction : {kDown, kUp}) {
    for (const auto knock : {kOut, kIn}) {
      for (const auto& [monitoring, treatment] : kTreatments) {
        cases.push_back({.name = std::string(direction == kDown ? "down" : "up") +
                                 (knock == kOut ? "-out " : "-in ") +
                                 TreatmentName(monitoring, treatment),
                         .contract = WithBarrier(direction, knock, monitoring, treatment, steps)});
      }
    }
  }
  return cases;
}

// The forward-mode market of tests/support/dual.hpp, one tangent per input in Market order.
inline pricer::Market<test::Dual> DualMarket(const pricer::Market<double>& m) {
  return {.spot = test::Dual::Variable(m.spot, 0),
          .sigma = test::Dual::Variable(m.sigma, 1),
          .r = test::Dual::Variable(m.r, 2),
          .q = test::Dual::Variable(m.q, 3),
          .maturity = test::Dual::Variable(m.maturity, 4)};
}

}  // namespace aadp::ad::testing

#endif  // AADP_TESTS_AD_CASES_HPP_
