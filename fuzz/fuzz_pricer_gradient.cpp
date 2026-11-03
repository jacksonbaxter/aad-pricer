// The pricer entry point on the tape (EVIDENCE B23), beside fuzz_pricer.cpp, whose input layout
// and seed corpus it shares: 88 bytes decode to a contract, a market, a seed and a thread count.
//
// Properties on a valid contract with at most kMaxPricedSteps steps:
// - PriceWithGradient succeeds, its value estimate is Price()'s bit for bit (mean, SE and every
//   batch mean), and every Greek's mean and SE are finite, the SE not negative;
// - each Greek's batch means, from one set-up sweep per block, equal the batch means of the same
//   paths' gradients taken one path at a time, within the round-off bound of check 7
//   (tests/ad/oracle_equivalence_test.cpp): so the block sweep, the zeroing between paths and
//   theta's sign are all judged on every input;
// - per path, on a few paths: the tape's value is the double kernel's bit for bit; its gradient
//   is finite; it agrees with the forward-mode Dual within check 1's association bound
//   (tests/ad/bounds.hpp), which covers vega of a European and every other Greek;
// - per path, on a barrier: knock-in plus knock-out equals the vanilla, Greek by Greek, within
//   the sum of the three paths' bounds and the rounding of the sum;
// - per path, on a European: delta of the call minus delta of the put is d S_T / S0, which by
//   put-call parity on the path is (C - P + d K) / S0, within both bounds and its own rounding.
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ad/bounds.hpp"  // tests/ad/, through the test support include path
#include "ad/gradient.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/dual_ops.hpp"  // IWYU pragma: keep (tests/pricer/: the Dual overloads, by ADL)
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "support/dual.hpp"

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
using aadp::ad::kInputs;
using aadp::ad::PathGradient;
using aadp::test::Dual;

constexpr std::size_t kInputBytes = 88;
constexpr std::uint64_t kPaths = 64;
constexpr std::uint32_t kBatches = 4;
constexpr std::uint64_t kCheckedPaths = 4;
// The tape is checked on shorter paths than fuzz_pricer.cpp prices, to keep each input fast.
constexpr std::uint32_t kMaxPricedSteps = 64;

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

// fuzz_pricer.cpp's layout.
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

bool SameBits(double a, double b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

void CheckEstimates(const Input& in) {
  const auto price = Price(in.contract, in.market, in.run);
  const auto gradient = aadp::ad::PriceWithGradient(in.contract, in.market, in.run);
  AADP_FUZZ_REQUIRE(price.has_value() && gradient.has_value(), "a valid contract was refused");
  AADP_FUZZ_REQUIRE(SameBits(price->mean, gradient->value.mean) &&
                        SameBits(price->standard_error, gradient->value.standard_error),
                    "tape value estimate differs from Price");
  for (std::size_t b = 0; b < kBatches; ++b) {
    AADP_FUZZ_REQUIRE(SameBits(price->batch_means[b], gradient->value.batch_means[b]),
                      "tape batch mean differs from Price");
  }
  for (const Estimate& greek : gradient->greeks) {
    AADP_FUZZ_REQUIRE(greek.batch_means.size() == kBatches, "greek batch count");
    AADP_FUZZ_REQUIRE(std::isfinite(greek.mean), "greek estimate not finite");
    AADP_FUZZ_REQUIRE(std::isfinite(greek.standard_error) && greek.standard_error >= 0.0,
                      "greek standard error not finite");
  }

  // The same paths one at a time. P paths per block, one block per batch here.
  const std::uint64_t per_batch = kPaths / kBatches;
  const auto p = static_cast<double>(per_batch);
  std::vector<double> normal(in.contract.steps);
  std::vector<double> crossing(in.contract.steps);
  const PathUniforms u{.normal = normal, .crossing = crossing};
  PathGradient path(in.contract, in.market);
  for (std::size_t b = 0; b < kBatches; ++b) {
    std::array<double, kInputs> sum{};
    std::array<double, kInputs> bound{};
    for (std::uint64_t i = 0; i < per_batch; ++i) {
      FillPath(in.run.seed, (b * per_batch) + i, normal, crossing);
      const PathGradient::PathResult r = path.Evaluate(u);
      const double m = (2.0 * static_cast<double>(r.statements)) +
                       ((p + 1.0) * static_cast<double>(r.operands)) + p + 6.0;
      const double gamma = aadp::ad::testing::Gamma(m);
      for (std::size_t k = 0; k < kInputs; ++k) {
        sum[k] += k == aadp::ad::kMaturityInput ? -r.gradient[k] : r.gradient[k];
        bound[k] += 2.0 * gamma * r.absolute[k] * (1.0 + (2.0 * gamma));
      }
    }
    for (std::size_t k = 0; k < kInputs; ++k) {
      const double one_at_a_time = sum[k] / p;
      const double blocked = gradient->greeks[k].batch_means[b];
      const double allowed = (bound[k] / p) + (p * 0x1p-1070) +
                             (0x1p-52 * (std::fabs(one_at_a_time) + std::fabs(blocked)));
      AADP_FUZZ_REQUIRE(std::fabs(blocked - one_at_a_time) <= allowed,
                        "greek batch mean differs from the paths taken one at a time");
    }
  }
}

struct Checked {
  PathGradient::PathResult tape;
  std::array<double, kInputs> bound{};
};

// One path of one contract on its own tape, against the double kernel and the Dual.
Checked CheckPath(const Contract& c, const Market<double>& m, const PathUniforms& u) {
  Checked out;
  const GbmModel<double> model(m, c.steps);
  const Market<Dual> dual_market{.spot = Dual::Variable(m.spot, 0),
                                 .sigma = Dual::Variable(m.sigma, 1),
                                 .r = Dual::Variable(m.r, 2),
                                 .q = Dual::Variable(m.q, 3),
                                 .maturity = Dual::Variable(m.maturity, 4)};
  const GbmModel<Dual> dual_model(dual_market, c.steps);
  const auto value = DiscountedPayoff<double>(c, model, u);
  const auto dual = DiscountedPayoff<Dual>(c, dual_model, u);
  {
    PathGradient path(c, m);
    out.tape = path.Evaluate(u);
  }
  AADP_FUZZ_REQUIRE(SameBits(out.tape.value, value), "tape path value differs from double");
  for (std::size_t i = 0; i < kInputs; ++i) {
    AADP_FUZZ_REQUIRE(std::isfinite(out.tape.gradient[i]), "path gradient not finite");
    out.bound[i] = aadp::ad::testing::AgreementBound(out.tape.absolute[i], out.tape.statements,
                                                     out.tape.operands);
    AADP_FUZZ_REQUIRE(std::fabs(out.tape.gradient[i] - dual.tangent[i]) <= out.bound[i],
                      "tape gradient differs from the Dual");
  }
  return out;
}

void CheckPaths(const Input& in) {
  const Contract& c = in.contract;
  std::vector<double> normal(c.steps);
  std::vector<double> crossing(c.steps);
  const PathUniforms u{.normal = normal, .crossing = crossing};
  for (std::uint64_t path = 0; path < kCheckedPaths; ++path) {
    FillPath(in.run.seed, path, normal, crossing);
    static_cast<void>(CheckPath(c, in.market, u));
    if (c.product == Product::kBarrier) {
      Contract vanilla = c;
      vanilla.product = Product::kEuropean;
      Contract out = c;
      out.barrier.knock = Knock::kOut;
      Contract knock_in = c;
      knock_in.barrier.knock = Knock::kIn;
      const Checked v = CheckPath(vanilla, in.market, u);
      const Checked o = CheckPath(out, in.market, u);
      const Checked k = CheckPath(knock_in, in.market, u);
      for (std::size_t i = 0; i < kInputs; ++i) {
        const double sum = k.tape.gradient[i] + o.tape.gradient[i];
        const double allowed = k.bound[i] + o.bound[i] + v.bound[i] +
                               (4.0 * aadp::ad::testing::kUnitRoundoff *
                                (std::fabs(k.tape.gradient[i]) + std::fabs(o.tape.gradient[i]) +
                                 std::fabs(v.tape.gradient[i])));
        AADP_FUZZ_REQUIRE(std::fabs(sum - v.tape.gradient[i]) <= allowed,
                          "knock-in plus knock-out gradient is not the vanilla's");
      }
    }
    if (c.product == Product::kEuropean) {
      // At S_T = K exactly both payoffs are the passive zero and the identity has no derivative.
      Contract call = c;
      call.type = OptionType::kCall;
      Contract put = c;
      put.type = OptionType::kPut;
      const Checked cp = CheckPath(call, in.market, u);
      const Checked pp = CheckPath(put, in.market, u);
      const double discount = GbmModel<double>(in.market, c.steps).Discount();
      const double dk = discount * c.strike;
      const double expected = (cp.tape.value - pp.tape.value + dk) / in.market.spot;
      const double rounding = 8.0 * aadp::ad::testing::kUnitRoundoff *
                              (cp.tape.value + pp.tape.value + dk) / in.market.spot;
      const double delta = cp.tape.gradient[0] - pp.tape.gradient[0];
      if (cp.tape.value == 0.0 && pp.tape.value == 0.0) continue;
      AADP_FUZZ_REQUIRE(std::fabs(delta - expected) <=
                            cp.bound[0] + pp.bound[0] + rounding +
                                (4.0 * aadp::ad::testing::kUnitRoundoff *
                                 (std::fabs(cp.tape.gradient[0]) + std::fabs(pp.tape.gradient[0]))),
                        "call delta minus put delta is not d S_T / S0");
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < kInputBytes) return 0;
  const Input in = Decode(data);
  const ContractError error = Validate(in.contract, in.market);
  if (error != ContractError::kNone) {
    const auto refused = aadp::ad::PriceWithGradient(in.contract, in.market, in.run);
    AADP_FUZZ_REQUIRE(!refused.has_value() && refused.error().contract == error,
                      "PriceWithGradient and Validate disagree");
    return 0;
  }
  if (in.contract.steps > kMaxPricedSteps) return 0;
  CheckEstimates(in);
  CheckPaths(in);
  return 0;
}
