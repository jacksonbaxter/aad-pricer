// The raw SVI fit (src/svi/fit.hpp) on small quote sets from raw bytes, every field free to take
// any value, NaN and infinities included:
//
//   offset  0  u8          quote count, mod 13 (0 to 12), cut to the quotes the input holds
//   offset  1  u8          unused
//   offset  2  u16         put mask: bit i set makes quote i a put
//   offset  4  u8 x 4      unused
//   offset  8  f64 x 3     forward, discount, expiry
//   offset 32  f64 x 3n    per quote: strike, price, noise
//   little-endian. An input shorter than 32 bytes is not a case.
//
// The search is smaller than the pre-registered one (3 x 3 grid, 2 starts, 40 evaluations per
// start, 20 inner steps), through the same code, so an input runs in milliseconds.
//
// Properties. Nothing throws, crashes or trips a sanitizer, and the result is either a fit or an
// explicit error:
// - an error has a name, and says what the input shows: a bad slice exactly when F, D or T is not
//   finite and positive; a bad quote, or a quote that does not invert, at an index inside the
//   slice, the latter with a named inversion status; too few strikes only with fewer than 5
//   distinct strikes. A fit needs at least 5.
// - a fit has one entry per quote in every per-quote field, finite and positive model vols and
//   finite, non-negative model prices; Phi is finite, non-negative and the sum of the squared
//   residuals it reports; the parameters lie inside every bound of the fit (b >= 0, |rho| <= 1,
//   b (1 + |rho|) <= 4, a <= a_max, m and sigma in their box), to 1e-9 relatively; the active
//   bounds are the ones flagged so; the starts are counted consistently; SVI-6 and SVI-11 are
//   finite.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>
#include <vector>

#include "svi/arbitrage.hpp"
#include "svi/fit.hpp"
#include "svi/implied_vol.hpp"

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

using namespace aadp::svi;

constexpr std::size_t kHeaderBytes = 32;
constexpr std::size_t kQuoteBytes = 24;
constexpr std::size_t kMaxQuotes = 12;
constexpr double kRelative = 1e-9;

constexpr FitOptions kOptions{.grid_m = 3,
                              .grid_sigma = 3,
                              .starts = 2,
                              .max_evaluations_per_start = 40,
                              .max_inner_steps = 20};

double ReadF64(const std::uint8_t* data, std::size_t offset) {
  double value = 0.0;
  std::memcpy(&value, data + offset, sizeof value);
  return value;
}

bool Named(std::string_view name) { return !name.empty() && name != "unknown"; }

bool Positive(double x) { return std::isfinite(x) && x > 0.0; }

bool ValidQuote(const FitQuote& q) {
  return Positive(q.strike) && Positive(q.price) && Positive(q.noise);
}

std::size_t DistinctStrikes(const FitSlice& slice) {
  std::vector<double> strikes;
  strikes.reserve(slice.quotes.size());
  for (const FitQuote& q : slice.quotes) strikes.push_back(q.strike);
  std::ranges::sort(strikes);
  return static_cast<std::size_t>(
      std::distance(strikes.begin(), std::ranges::unique(strikes).begin()));
}

bool AtMost(double x, double bound) {
  return x <= bound + (kRelative * std::max(std::fabs(bound), std::fabs(x)));
}

void CheckError(const FitSlice& slice, const FitError& e) {
  AADP_FUZZ_REQUIRE(e.code <= FitErrorCode::kNoFeasibleStart, "known error code");
  AADP_FUZZ_REQUIRE(Named(FitErrorName(e.code)), "error has a name");
  const bool good_slice =
      Positive(slice.forward) && Positive(slice.discount) && Positive(slice.expiry);
  AADP_FUZZ_REQUIRE((e.code == FitErrorCode::kBadSlice) == !good_slice, "bad slice when it is");
  switch (e.code) {
    case FitErrorCode::kBadSlice:
    case FitErrorCode::kNoFeasibleStart:
      break;
    case FitErrorCode::kBadQuote:
      AADP_FUZZ_REQUIRE(e.quote < slice.quotes.size(), "bad quote inside the slice");
      AADP_FUZZ_REQUIRE(!ValidQuote(slice.quotes[e.quote]), "bad quote is bad");
      break;
    case FitErrorCode::kTooFewStrikes:
      AADP_FUZZ_REQUIRE(DistinctStrikes(slice) < 5, "too few strikes when there are");
      break;
    case FitErrorCode::kNoImpliedVol:
      AADP_FUZZ_REQUIRE(e.quote < slice.quotes.size(), "no-vol quote inside the slice");
      AADP_FUZZ_REQUIRE(e.status != InversionStatus::kOk && Named(StatusName(e.status)),
                        "no-vol status named");
      break;
  }
}

void CheckFit(const FitSlice& slice, const FitResult& r) {
  const std::size_t n = slice.quotes.size();
  AADP_FUZZ_REQUIRE(DistinctStrikes(slice) >= 5, "a fit has 5 strikes");
  AADP_FUZZ_REQUIRE(r.quotes == n, "quote count");
  AADP_FUZZ_REQUIRE(r.market_vol.size() == n && r.model_vol.size() == n &&
                        r.model_price.size() == n && r.residual.size() == n,
                    "per-quote sizes");
  double sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    AADP_FUZZ_REQUIRE(Positive(r.market_vol[i]), "market vol finite and positive");
    AADP_FUZZ_REQUIRE(Positive(r.model_vol[i]), "model vol finite and positive");
    AADP_FUZZ_REQUIRE(std::isfinite(r.model_price[i]) && r.model_price[i] >= 0.0,
                      "model price finite and non-negative");
    AADP_FUZZ_REQUIRE(std::isfinite(r.residual[i]), "residual finite");
    sum += r.residual[i] * r.residual[i];
  }
  AADP_FUZZ_REQUIRE(std::isfinite(r.objective) && r.objective >= 0.0, "Phi finite");
  AADP_FUZZ_REQUIRE(std::fabs(sum - r.objective) <= 1e-12 * std::max(sum, r.objective),
                    "Phi is the sum of the squared residuals");

  const SviParams& q = r.params;
  AADP_FUZZ_REQUIRE(std::isfinite(q.a) && std::isfinite(q.b) && std::isfinite(q.rho) &&
                        std::isfinite(q.m) && std::isfinite(q.sigma),
                    "parameters finite");
  AADP_FUZZ_REQUIRE(q.b >= 0.0 && std::fabs(q.rho) <= 1.0, "b >= 0 and |rho| <= 1");
  AADP_FUZZ_REQUIRE(AtMost(q.b * (1.0 + std::fabs(q.rho)), kOptions.slope_bound), "slope bound");
  AADP_FUZZ_REQUIRE(AtMost(q.a, r.a_max), "a <= a_max");
  AADP_FUZZ_REQUIRE(r.m_lo <= q.m && q.m <= r.m_hi, "m in its box");
  AADP_FUZZ_REQUIRE(AtMost(r.sigma_lo, q.sigma) && AtMost(q.sigma, r.sigma_hi), "sigma in its box");

  std::vector<Bound> flagged;
  for (std::size_t i = 0; i < kBoundCount; ++i) {
    AADP_FUZZ_REQUIRE(static_cast<std::size_t>(r.bounds[i].bound) == i, "bounds in order");
    AADP_FUZZ_REQUIRE(Named(BoundName(r.bounds[i].bound)), "bound has a name");
    if (r.bounds[i].active) flagged.push_back(r.bounds[i].bound);
  }
  AADP_FUZZ_REQUIRE(r.ActiveBounds() == flagged, "active bounds are the flagged ones");
  AADP_FUZZ_REQUIRE(r.starts >= 1 && r.starts <= kOptions.starts, "starts counted");
  AADP_FUZZ_REQUIRE(r.starts_at_best >= 1 && r.starts_at_best <= r.starts, "starts at best");
  AADP_FUZZ_REQUIRE(r.profile_evaluations >= 1, "profile evaluated");
  AADP_FUZZ_REQUIRE(std::isfinite(MeanAbsPriceErrorBp(slice, r.model_price)), "SVI-6 finite");
  AADP_FUZZ_REQUIRE(std::isfinite(ImpliedVolRmse(r.model_vol, r.market_vol)), "SVI-11 finite");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < kHeaderBytes) return 0;
  const std::size_t n =
      std::min<std::size_t>(data[0] % (kMaxQuotes + 1), (size - kHeaderBytes) / kQuoteBytes);
  const unsigned puts = data[2] | (static_cast<unsigned>(data[3]) << 8U);
  FitSlice slice{.forward = ReadF64(data, 8),
                 .discount = ReadF64(data, 16),
                 .expiry = ReadF64(data, 24),
                 .quotes = {}};
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t offset = kHeaderBytes + (kQuoteBytes * i);
    slice.quotes.push_back({.strike = ReadF64(data, offset),
                            .price = ReadF64(data, offset + 8),
                            .type = ((puts >> i) & 1U) != 0 ? OptionType::kPut : OptionType::kCall,
                            .noise = ReadF64(data, offset + 16)});
  }
  const auto fit = FitRawSvi(slice, kOptions);
  if (fit) {
    CheckFit(slice, *fit);
  } else {
    CheckError(slice, fit.error());
  }
  return 0;
}
