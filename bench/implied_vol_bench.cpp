// SVI-2, iteration only: the time of one Black-76 implied-volatility inversion through
// ImpliedVolatility (Let's Be Rational, src/svi/implied_vol.hpp), and of one Black76Price for
// scale. Not a reported result: it has no stamped results file yet, so every run is labelled
// "iteration only". BENCHMARKS.md quotes one such run under SVI-2, labelled iteration only, until
// the measured run at stage 11.
//
// The quotes: F = 100, D = exp(-0.03 T), T in {0.0137, 0.0833, 0.5, 2}, sigma in {0.1, 0.2, 0.4,
// 0.8}, z = ln(K/F) / (sigma sqrt(T)) from -3 to 3 in steps of 0.25, out-of-the-money calls and
// puts (calls for z >= 0): 400 quotes, the range where quoted options live. Each iteration
// inverts all of them, so the time per item is the mean over the set.
#include <benchmark/benchmark.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "svi/implied_vol.hpp"

namespace {

using aadp::svi::Black76Quote;
using aadp::svi::OptionType;

constexpr const char* kLabel = "iteration only";

std::vector<Black76Quote> Quotes() {
  std::vector<Black76Quote> quotes;
  for (const double t : {0.0137, 0.0833, 0.5, 2.0}) {
    for (const double vol : {0.1, 0.2, 0.4, 0.8}) {
      for (int i = -12; i <= 12; ++i) {
        const double z = 0.25 * i;
        const double strike = 100.0 * std::exp(z * vol * std::sqrt(t));
        const double discount = std::exp(-0.03 * t);
        const OptionType type = z >= 0.0 ? OptionType::kCall : OptionType::kPut;
        quotes.push_back({.forward = 100.0,
                          .discount = discount,
                          .strike = strike,
                          .expiry = t,
                          .price = aadp::svi::Black76Price(100.0, discount, strike, t, vol, type),
                          .type = type});
      }
    }
  }
  return quotes;
}

void BM_ImpliedVolatility(benchmark::State& state) {
  const std::vector<Black76Quote> quotes = Quotes();
  for (const Black76Quote& q : quotes) {
    if (aadp::svi::ImpliedVolatility(q).status != aadp::svi::InversionStatus::kOk) {
      state.SkipWithError("a benchmark quote did not invert");
      return;
    }
  }
  for ([[maybe_unused]] const auto _ : state) {
    for (const Black76Quote& q : quotes) {
      benchmark::DoNotOptimize(aadp::svi::ImpliedVolatility(q));
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(quotes.size()));
  state.SetLabel(kLabel);
}
BENCHMARK(BM_ImpliedVolatility);

void BM_Black76Price(benchmark::State& state) {
  const std::vector<Black76Quote> quotes = Quotes();
  std::vector<double> vols;
  vols.reserve(quotes.size());
  for (const Black76Quote& q : quotes) vols.push_back(aadp::svi::ImpliedVolatility(q).vol);
  for ([[maybe_unused]] const auto _ : state) {
    for (std::size_t i = 0; i < quotes.size(); ++i) {
      const Black76Quote& q = quotes[i];
      benchmark::DoNotOptimize(
          aadp::svi::Black76Price(q.forward, q.discount, q.strike, q.expiry, vols[i], q.type));
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(quotes.size()));
  state.SetLabel(kLabel);
}
BENCHMARK(BM_Black76Price);

}  // namespace
