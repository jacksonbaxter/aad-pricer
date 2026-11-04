// Every product and treatment the pricer has, for the SIMD agreement tests: each product with
// both option types, the Asians with and without the spot, and every valid barrier treatment in
// both directions, knock-out and knock-in.
#ifndef AADP_TESTS_SIMD_CASES_HPP_
#define AADP_TESTS_SIMD_CASES_HPP_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "pricer/contract.hpp"

namespace aadp::simd::testing {

struct Case {
  std::string name;
  pricer::Contract contract;
};

inline const char* TypeName(pricer::OptionType type) {
  return type == pricer::OptionType::kCall ? "call" : "put";
}

inline void AddPlainCases(pricer::OptionType type, std::uint32_t steps, std::vector<Case>& out) {
  using pricer::Product;
  struct Plain {
    Product product;
    const char* name;
    bool include_spot;
  };
  for (const Plain& p : std::to_array<Plain>({
           {.product = Product::kEuropean, .name = "european", .include_spot = false},
           {.product = Product::kDigital, .name = "digital", .include_spot = false},
           {.product = Product::kAsianArithmetic,
            .name = "arithmetic asian",
            .include_spot = false},
           {.product = Product::kAsianArithmetic,
            .name = "arithmetic asian with spot",
            .include_spot = true},
           {.product = Product::kAsianGeometric, .name = "geometric asian", .include_spot = false},
           {.product = Product::kAsianGeometric,
            .name = "geometric asian with spot",
            .include_spot = true},
       })) {
    pricer::Contract c;
    c.product = p.product;
    c.type = type;
    c.strike = 100.0;
    c.steps = steps;
    c.include_spot = p.include_spot;
    out.push_back({.name = std::string(p.name) + " " + TypeName(type), .contract = c});
  }
}

inline void AddBarrierCases(pricer::OptionType type, std::uint32_t steps, std::vector<Case>& out) {
  using namespace pricer;
  struct Treated {
    Monitoring monitoring;
    Treatment treatment;
    const char* name;
  };
  for (const Treated& t : std::to_array<Treated>({
           {.monitoring = Monitoring::kContinuous,
            .treatment = Treatment::kBridge,
            .name = "bridge"},
           {.monitoring = Monitoring::kContinuous,
            .treatment = Treatment::kHard,
            .name = "sampled crossing"},
           {.monitoring = Monitoring::kDiscrete,
            .treatment = Treatment::kOneStep,
            .name = "one-step"},
           {.monitoring = Monitoring::kDiscrete, .treatment = Treatment::kHard, .name = "hard"},
           {.monitoring = Monitoring::kDiscrete,
            .treatment = Treatment::kSmoothed,
            .name = "smoothed"},
       })) {
    for (const Direction direction : {Direction::kDown, Direction::kUp}) {
      for (const Knock knock : {Knock::kOut, Knock::kIn}) {
        Contract c;
        c.product = Product::kBarrier;
        c.type = type;
        c.strike = 100.0;
        c.steps = steps;
        const bool down = direction == Direction::kDown;
        c.barrier = {.direction = direction,
                     .knock = knock,
                     .monitoring = t.monitoring,
                     .treatment = t.treatment,
                     .level = down ? 92.0 : 108.0,
                     .smoothing_width = t.treatment == Treatment::kSmoothed ? 0.01 : 0.0};
        out.push_back({.name = std::string(down ? "down-" : "up-") +
                               (knock == Knock::kOut ? "out " : "in ") + t.name + " " +
                               TypeName(type),
                       .contract = c});
      }
    }
  }
}

inline std::vector<Case> AllCases(std::uint32_t steps) {
  std::vector<Case> out;
  for (const pricer::OptionType type : {pricer::OptionType::kCall, pricer::OptionType::kPut}) {
    AddPlainCases(type, steps, out);
    AddBarrierCases(type, steps, out);
  }
  return out;
}

}  // namespace aadp::simd::testing

#endif  // AADP_TESTS_SIMD_CASES_HPP_
