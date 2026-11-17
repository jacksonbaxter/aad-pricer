// SVI-2: the time of one Black-76 implied-volatility inversion through ImpliedVolatility (Let's
// Be Rational, src/svi/implied_vol.hpp), and of one Black76Price for scale. Through the harness
// main (harness.hpp), so it writes a benchmark results file with the run header and takes the run
// flags; scripts/run_bench.sh runs and stamps it.
//
// The quotes: F = 100, D = exp(-0.03 T), T in {0.0137, 0.0833, 0.5, 2}, sigma in {0.1, 0.2, 0.4,
// 0.8}, z = ln(K/F) / (sigma sqrt(T)) from -3 to 3 in steps of 0.25, out-of-the-money calls and
// puts (calls for z >= 0): 400 quotes, the range where quoted options live. One unit inverts (or
// prices) all of them once, and a batch is one unit, so a Google Benchmark iteration is one pass
// over the set, as it was before the harness, and the time per item is the mean over the set.
// The names are the bench's names before the harness in lower case: a results section name has
// no capitals.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "harness.hpp"
#include "svi/implied_vol.hpp"

namespace {

using aadp::svi::Black76Quote;
using aadp::svi::OptionType;

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

// Every quote must invert before anything is timed, as the bench required before the harness.
bool AllInvert(const std::vector<Black76Quote>& quotes) {
  return std::ranges::all_of(quotes, [](const Black76Quote& q) {
    return aadp::svi::ImpliedVolatility(q).status == aadp::svi::InversionStatus::kOk;
  });
}

// The timed loops, each pass the loop body the bench had before the harness.
void InvertAll(const std::vector<Black76Quote>& quotes, std::uint64_t units) {
  for (std::uint64_t unit = 0; unit < units; ++unit) {
    for (const Black76Quote& q : quotes) {
      benchmark::DoNotOptimize(aadp::svi::ImpliedVolatility(q));
    }
  }
}

void PriceAll(const std::vector<Black76Quote>& quotes, const std::vector<double>& vols,
              std::uint64_t units) {
  for (std::uint64_t unit = 0; unit < units; ++unit) {
    for (std::size_t i = 0; i < quotes.size(); ++i) {
      const Black76Quote& q = quotes[i];
      benchmark::DoNotOptimize(
          aadp::svi::Black76Price(q.forward, q.discount, q.strike, q.expiry, vols[i], q.type));
    }
  }
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  const auto quotes = std::make_shared<const std::vector<Black76Quote>>(Quotes());
  if (!AllInvert(*quotes)) {
    std::fprintf(stderr, "aadp_bench_implied_vol: a benchmark quote did not invert\n");
    return aadp::bench::kExitFailure;
  }
  const auto vols = std::make_shared<std::vector<double>>();
  vols->reserve(quotes->size());
  for (const Black76Quote& q : *quotes) vols->push_back(aadp::svi::ImpliedVolatility(q).vol);

  std::vector<aadp::bench::Workload> workloads;
  workloads.push_back({.name = "bm_impliedvolatility",
                       .run = [quotes](std::uint64_t units) { InvertAll(*quotes, units); },
                       .units = 1});
  workloads.push_back(
      {.name = "bm_black76price",
       .run = [quotes, vols](std::uint64_t units) { PriceAll(*quotes, *vols, units); },
       .units = 1});

  aadp::bench::RunExtras extras;
  extras.claim = "result";
  const auto add = [&extras](std::string key, std::string value) {
    extras.header.push_back({.key = std::move(key), .value = std::move(value)});
  };
  add("bench_quotes",
      std::to_string(quotes->size()) +
          " out-of-the-money Black-76 quotes: F 100, D exp(-0.03 T), T 0.0137, 0.0833, 0.5 and 2, "
          "sigma 0.1, 0.2, 0.4 and 0.8, z = ln(K/F) / (sigma sqrt(T)) from -3 to 3 by 0.25, calls "
          "for z >= 0");
  add("bench_unit",
      "one unit inverts every quote once (bm_impliedvolatility) or prices every quote once at its "
      "inverted vol (bm_black76price); a batch is one unit, so the time per quote is a batch's "
      "over the quotes");
  add("bench_inversion_check", "pass: every quote inverts with status ok");
  return aadp::bench::harness_main(argc, argv, std::move(workloads), extras);
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_bench_implied_vol: %s\n", error.what());
  return aadp::bench::kExitFailure;
}
