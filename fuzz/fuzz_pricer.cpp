// Pricer entry point (EVIDENCE B23). The input is a contract and a market as raw bytes, so every
// field can take any value, NaN and infinities included:
//
//   offset  0  u8 x 8   product, option type, direction, knock, monitoring, treatment,
//                       include_spot (low bit), threads (1 + byte % 3)
//   offset  8  u32      steps
//   offset 12  u32      unused
//   offset 16  u64      seed
//   offset 24  f64 x 8  spot, strike, barrier level, smoothing width, sigma, r, q, T
//   88 bytes, little-endian. A shorter input is not a contract.
//
// Properties. Validate and Price agree, and Price refuses every invalid contract with an error
// value that has a name. On a valid one, at a small fixed path count:
// - every batch mean, the estimate and its SE are finite, and the SE is not negative;
// - prices are not negative, except a one-step knock-in, whose single path may be (its knock-out
//   leg runs on its own conditioned path);
// - per path: the value is finite, the weight in [0, 1]; a put and a knock-out put at most the
//   discounted strike, a digital at most the discount; a knock-out other than one-step at most
//   the vanilla on the same path; knock-in plus knock-out parity exact; call and put on the same
//   path satisfy put-call parity, which with both non-negative is the per-path no-arbitrage bound
//   max(d (S - K), 0) <= C <= d S.
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"

// A failed property aborts, so libFuzzer keeps the input, and says which one failed.
#define AADP_FUZZ_REQUIRE(condition, what)                                                   \
  do {                                                                                       \
    if (!(condition)) {                                                                      \
      std::fprintf(stderr, "invariant failed: %s: %s (%s:%d)\n", what, #condition, __FILE__, \
                   __LINE__);                                                                \
      std::abort();                                                                          \
    }                                                                                        \
  } while (false)

namespace {

using namespace aadp::pricer;

constexpr std::size_t kInputBytes = 88;
constexpr std::uint64_t kPaths = 64;
constexpr std::uint32_t kBatches = 4;
constexpr std::uint64_t kCheckedPaths = 8;
// Beyond this many steps only validation runs, to keep each input fast.
constexpr std::uint32_t kMaxPricedSteps = 128;

struct Input {
  Contract contract;
  Market<double> market{};
  RunConfig run;
};

template <class T>
T Read(const std::uint8_t* data, std::size_t offset) {
  T value;
  std::memcpy(&value, data + offset, sizeof(T));
  return value;
}

Input Decode(const std::uint8_t* data) {
  Input in;
  Contract& c = in.contract;
  c.product = static_cast<Product>(data[0]);
  c.type = static_cast<OptionType>(data[1]);
  c.barrier.direction = static_cast<Direction>(data[2]);
  c.barrier.knock = static_cast<Knock>(data[3]);
  c.barrier.monitoring = static_cast<Monitoring>(data[4]);
  c.barrier.treatment = static_cast<Treatment>(data[5]);
  c.include_spot = (data[6] & 1U) != 0;
  c.steps = Read<std::uint32_t>(data, 8);
  const auto field = [&](std::size_t i) { return Read<double>(data, 24 + (8 * i)); };
  in.market.spot = field(0);
  c.strike = field(1);
  c.barrier.level = field(2);
  c.barrier.smoothing_width = field(3);
  in.market.sigma = field(4);
  in.market.r = field(5);
  in.market.q = field(6);
  in.market.maturity = field(7);
  in.run = {.seed = Read<std::uint64_t>(data, 16),
            .paths = kPaths,
            .batches = kBatches,
            .threads = 1U + (data[7] % 3U)};
  return in;
}

bool OneStepKnockIn(const Contract& c) {
  return c.product == Product::kBarrier && c.barrier.knock == Knock::kIn &&
         c.barrier.treatment == Treatment::kOneStep;
}

void CheckEstimate(const Input& in, const Estimate& estimate) {
  AADP_FUZZ_REQUIRE(estimate.batch_means.size() == kBatches, "batch count");
  for (const double mean : estimate.batch_means) {
    AADP_FUZZ_REQUIRE(std::isfinite(mean), "batch mean not finite");
    if (!OneStepKnockIn(in.contract)) AADP_FUZZ_REQUIRE(mean >= 0.0, "negative price");
  }
  AADP_FUZZ_REQUIRE(std::isfinite(estimate.mean), "price not finite");
  AADP_FUZZ_REQUIRE(std::isfinite(estimate.standard_error) && estimate.standard_error >= 0.0,
                    "standard error not finite");
}

Contract WithType(Contract c, OptionType type) {
  c.type = type;
  return c;
}

void CheckPaths(const Input& in) {
  const Contract& c = in.contract;
  const GbmModel<double> model(in.market, c.steps);
  const double discount = model.Discount();
  const double strike_bound = discount * c.strike;
  std::vector<double> normal(c.steps);
  std::vector<double> crossing(c.steps);
  const PathUniforms u{.normal = normal, .crossing = crossing};
  Contract vanilla = c;
  vanilla.product = Product::kEuropean;
  Contract out = c;
  out.barrier.knock = Knock::kOut;
  Contract in_contract = c;
  in_contract.barrier.knock = Knock::kIn;
  for (std::uint64_t path = 0; path < kCheckedPaths; ++path) {
    FillPath(in.run.seed, path, normal, crossing);
    const PathValue<double> v = EvaluatePath<double>(c, model, u);
    AADP_FUZZ_REQUIRE(std::isfinite(v.value), "path value not finite");
    AADP_FUZZ_REQUIRE(v.weight >= 0.0 && v.weight <= 1.0, "weight outside [0, 1]");
    if (!OneStepKnockIn(c)) AADP_FUZZ_REQUIRE(v.value >= 0.0, "negative path value");
    if (c.product == Product::kDigital) {
      AADP_FUZZ_REQUIRE(v.value <= discount, "digital above the discount");
    } else if (c.type == OptionType::kPut && !OneStepKnockIn(c)) {
      AADP_FUZZ_REQUIRE(v.value <= strike_bound, "put above the discounted strike");
    }
    if (c.product == Product::kBarrier) {
      const double vanilla_value = DiscountedPayoff<double>(vanilla, model, u);
      const double out_value = DiscountedPayoff<double>(out, model, u);
      const double in_value = DiscountedPayoff<double>(in_contract, model, u);
      AADP_FUZZ_REQUIRE(std::bit_cast<std::uint64_t>(in_value) ==
                            std::bit_cast<std::uint64_t>(vanilla_value - out_value),
                        "knock-in parity");
      if (c.barrier.treatment != Treatment::kOneStep) {
        AADP_FUZZ_REQUIRE(out_value <= vanilla_value, "knock-out above the vanilla");
      }
    }
    if (c.product == Product::kEuropean) {
      // C - P = d fl(S - K) exactly, and the same at K' = 2K, so the difference is d (K' - K) to
      // within the roundings of the two subtractions and their products.
      Contract wide = c;
      wide.strike = 2.0 * c.strike;
      const double call = DiscountedPayoff<double>(WithType(c, OptionType::kCall), model, u);
      const double put = DiscountedPayoff<double>(WithType(c, OptionType::kPut), model, u);
      const double call_wide =
          DiscountedPayoff<double>(WithType(wide, OptionType::kCall), model, u);
      const double put_wide = DiscountedPayoff<double>(WithType(wide, OptionType::kPut), model, u);
      AADP_FUZZ_REQUIRE(call >= 0.0 && put >= 0.0, "negative vanilla");
      const double gap = (call - put) - (call_wide - put_wide);
      const double rounding =
          8.0 * 0x1p-53 * (call + put + call_wide + put_wide + (discount * wide.strike));
      AADP_FUZZ_REQUIRE(std::fabs(gap - (discount * c.strike)) <= rounding + 0x1p-1022,
                        "put-call parity");
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < kInputBytes) return 0;
  const Input in = Decode(data);
  const ContractError error = Validate(in.contract, in.market);
  AADP_FUZZ_REQUIRE(std::string_view(ErrorName(error)) != "unknown error", "error has no name");
  if (error == ContractError::kNone && in.contract.steps > kMaxPricedSteps) return 0;
  const auto estimate = Price(in.contract, in.market, in.run);
  if (error != ContractError::kNone) {
    AADP_FUZZ_REQUIRE(!estimate.has_value(), "an invalid contract was priced");
    AADP_FUZZ_REQUIRE(estimate.error().contract == error, "Price and Validate disagree");
    return 0;
  }
  AADP_FUZZ_REQUIRE(estimate.has_value(), "a valid contract was refused");
  CheckEstimate(in, *estimate);
  CheckPaths(in);
  return 0;
}
