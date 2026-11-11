#include "f32/paired.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <thread>
#include <vector>

#include "f32/cpu_pricer.hpp"
#include "f32/float_kernels.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/runner.hpp"
#include "rng/philox.hpp"

namespace aadp::f32 {

namespace {

constexpr std::uint64_t kSpansPerChunk = 64;

}  // namespace

double UniformFromWord(std::uint32_t word, UniformBits bits) {
  if (bits == UniformBits::k32) return (static_cast<double>(word) + 0.5) * 0x1p-32;
  return (static_cast<double>(word >> 9U) + 0.5) * 0x1p-23;
}

std::expected<Column, SetupError> PriceSpansDouble(const pricer::Contract& contract,
                                                   const pricer::Market<double>& market,
                                                   std::uint64_t seed, std::uint64_t spans,
                                                   std::uint32_t span, UniformBits bits,
                                                   std::uint32_t threads) {
  if (pricer::Validate(contract, market) != pricer::ContractError::kNone) {
    return std::unexpected(SetupError::kContract);
  }
  if (pricer::NeedsCrossingUniforms(contract)) return std::unexpected(SetupError::kProduct);
  if (span < 1 || span > kMaxSpan) return std::unexpected(SetupError::kSpan);
  Column out;
  out.span = span;
  out.sum.resize(static_cast<std::size_t>(spans));
  out.sum_squares.resize(static_cast<std::size_t>(spans));
  const pricer::GbmModel<double> model(market, contract.steps);
  const Philox4x32Key key = {static_cast<std::uint32_t>(seed),
                             static_cast<std::uint32_t>(seed >> 32U)};
  const std::uint64_t chunks = (spans + kSpansPerChunk - 1) / kSpansPerChunk;
  std::atomic<std::uint64_t> next{0};
  const auto worker = [&] {
    std::vector<double> normal(contract.steps);
    const pricer::PathUniforms uniforms{.normal = normal, .crossing = {}};
    for (;;) {
      const std::uint64_t chunk = next.fetch_add(1, std::memory_order_relaxed);
      if (chunk >= chunks) return;
      const std::uint64_t last = std::min(spans, (chunk + 1) * kSpansPerChunk);
      for (std::uint64_t s = chunk * kSpansPerChunk; s < last; ++s) {
        double sum = 0.0;
        double squares = 0.0;
        for (std::uint64_t i = 0; i < span; ++i) {
          const std::uint64_t path = s * span + i;
          for (std::uint32_t step = 0; step < contract.steps; ++step) {
            const Philox4x32Output block =
                Philox4x32_10({static_cast<std::uint32_t>(path),
                               static_cast<std::uint32_t>(path >> 32U), step, 0U},
                              key);
            normal[step] = UniformFromWord(block[0], bits);
          }
          const auto value = pricer::DiscountedPayoff<double>(contract, model, uniforms);
          sum += value;
          squares += value * value;
        }
        out.sum[static_cast<std::size_t>(s)] = sum;
        out.sum_squares[static_cast<std::size_t>(s)] = squares;
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

Column FromFloat(std::span<const SpanSum> spans, std::uint32_t span, double scale) {
  Column out;
  out.span = span;
  out.sum.reserve(spans.size());
  out.sum_squares.reserve(spans.size());
  for (const SpanSum& s : spans) {
    out.sum.push_back(scale * static_cast<double>(s.sum));
    out.sum_squares.push_back(scale * scale * static_cast<double>(s.sum_squares));
  }
  return out;
}

Estimate ColumnEstimate(const Column& column) {
  Moments m;
  for (std::size_t i = 0; i < column.sum.size(); ++i) {
    m.sum += column.sum[i];
    m.sum_squares += column.sum_squares[i];
  }
  m.paths = static_cast<std::uint64_t>(column.sum.size()) * column.span;
  return ToEstimate(m, 1.0);
}

Difference PairedDifference(const Column& a, const Column& b) {
  const std::size_t spans = std::min(a.sum.size(), b.sum.size());
  const auto k = static_cast<double>(spans);
  double total = 0.0;
  for (std::size_t i = 0; i < spans; ++i) total += a.sum[i] - b.sum[i];
  const double mean_span = total / k;
  double squares = 0.0;
  for (std::size_t i = 0; i < spans; ++i) {
    const double d = (a.sum[i] - b.sum[i]) - mean_span;
    squares += d * d;
  }
  const double span = a.span;
  const double sd_span = std::sqrt(squares / (k - 1.0));
  return {.mean = mean_span / span, .standard_error = sd_span / (span * std::sqrt(k))};
}

}  // namespace aadp::f32
