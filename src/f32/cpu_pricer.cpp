#include "f32/cpu_pricer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <thread>
#include <vector>

#include "f32/float_kernels.hpp"
#include "pricer/contract.hpp"
#include "pricer/runner.hpp"

namespace aadp::f32 {

namespace {

// Spans per unit of work a CPU worker takes. Each span's slot is written once, so the result does
// not depend on which worker ran it.
constexpr std::uint64_t kSpansPerChunk = 64;

}  // namespace

std::expected<Params, SetupError> MakeParams(const pricer::Contract& contract,
                                             const pricer::Market<double>& market,
                                             std::uint64_t seed, std::uint32_t span) {
  if (pricer::Validate(contract, market) != pricer::ContractError::kNone) {
    return std::unexpected(SetupError::kContract);
  }
  if (span < 1 || span > kMaxSpan) return std::unexpected(SetupError::kSpan);
  Params p;
  if (contract.product == pricer::Product::kAsianArithmetic && !contract.include_spot) {
    p.product = static_cast<Word>(Product::kAsian);
  } else if (contract.product == pricer::Product::kBarrier &&
             contract.barrier.knock == pricer::Knock::kOut &&
             contract.barrier.monitoring == pricer::Monitoring::kDiscrete &&
             contract.barrier.treatment == pricer::Treatment::kOneStep) {
    p.product = static_cast<Word>(contract.barrier.direction == pricer::Direction::kUp
                                      ? Product::kUpOutOneStep
                                      : Product::kDownOutOneStep);
    p.barrier = static_cast<float>(std::log(contract.barrier.level / market.spot));
  } else {
    return std::unexpected(SetupError::kProduct);
  }
  // float(m) must be exact for the average's divisor; Validate caps m far below 2^24.
  if (contract.steps >= (1U << 24U)) return std::unexpected(SetupError::kContract);
  p.put = contract.type == pricer::OptionType::kPut ? 1U : 0U;
  p.steps = contract.steps;
  p.span = span;
  p.seed_lo = static_cast<Word>(seed);
  p.seed_hi = static_cast<Word>(seed >> 32U);
  const double dt = market.maturity / static_cast<double>(contract.steps);
  p.drift = static_cast<float>((market.r - market.q - 0.5 * market.sigma * market.sigma) * dt);
  p.sd = static_cast<float>(market.sigma * std::sqrt(dt));
  p.strike = static_cast<float>(contract.strike / market.spot);
  return p;
}

const char* ErrorName(SetupError error) {
  switch (error) {
    case SetupError::kNone:
      return "none";
    case SetupError::kContract:
      return "the contract or market does not validate";
    case SetupError::kProduct:
      return "not an arithmetic Asian without S0 or a discrete one-step knock-out";
    case SetupError::kSpan:
      return "span outside 1 to 64";
  }
  return "unknown";
}

double Scale(const pricer::Market<double>& market) {
  return market.spot * std::exp(-market.r * market.maturity);
}

std::vector<SpanSum> PriceSpansCpu(const Params& params, std::uint64_t first_path,
                                   std::uint64_t spans, std::uint32_t threads) {
  std::vector<SpanSum> out(static_cast<std::size_t>(spans));
  const std::uint64_t chunks = (spans + kSpansPerChunk - 1) / kSpansPerChunk;
  std::atomic<std::uint64_t> next{0};
  const auto worker = [&] {
    for (;;) {
      const std::uint64_t chunk = next.fetch_add(1, std::memory_order_relaxed);
      if (chunk >= chunks) return;
      const std::uint64_t last = std::min(spans, (chunk + 1) * kSpansPerChunk);
      for (std::uint64_t s = chunk * kSpansPerChunk; s < last; ++s) {
        const std::uint64_t first = first_path + s * params.span;
        out[static_cast<std::size_t>(s)] =
            PriceSpan(params, static_cast<Word>(first), static_cast<Word>(first >> 32U));
      }
    }
  };
  const std::uint32_t count = std::clamp<std::uint32_t>(threads, 1, pricer::kMaxThreads);
  {
    std::vector<std::jthread> pool;
    pool.reserve(count - 1);
    for (std::uint32_t t = 1; t < count; ++t) pool.emplace_back(worker);
    worker();
  }
  return out;
}

Moments Reduce(std::span<const SpanSum> spans, std::uint32_t span) {
  Moments m;
  for (const SpanSum& s : spans) {
    m.sum += static_cast<double>(s.sum);
    m.sum_squares += static_cast<double>(s.sum_squares);
  }
  m.paths = static_cast<std::uint64_t>(spans.size()) * span;
  return m;
}

Estimate ToEstimate(const Moments& moments, double scale) {
  const auto n = static_cast<double>(moments.paths);
  const double mean = moments.sum / n;
  const double variance = std::max(0.0, (moments.sum_squares - n * mean * mean) / (n - 1.0));
  return {.mean = scale * mean, .standard_error = scale * std::sqrt(variance / n)};
}

}  // namespace aadp::f32
