// The eSSVI surface fit (src/svi/essvi_fit.hpp) and its global map (src/svi/essvi.hpp) from raw
// bytes, every slice field free to take any value, NaN and infinities included:
//
//   offset  0  u8          slice count, mod 3, plus 1 (1 to 3)
//   offset  1  u8 x 3      quote count per slice, mod 13 (0 to 12), cut to the quotes the input
//   holds offset  4  u16 x 3     put mask per slice: bit i set makes quote i a put offset 10  u8 x
//   2      unused offset 12  u32 x 9     a box point, as fractions u / 2^32 of each coordinate's
//   range offset 48  per slice:  f64 x 3 (forward, discount, expiry), then f64 x 3 per quote
//   (strike,
//                          price, noise)
//   little-endian. An input shorter than 48 bytes is not a case; a slice whose header the input
//   does not hold is dropped, and with no slice left there is no fit.
//
// The search is smaller than the pre-registered one (2 random starts, 60 iterations), through the
// same code, so an input runs in milliseconds.
//
// Properties (docs/NOTES.md, "Stage 7c"). Nothing throws, crashes or trips a sanitizer, and the
// result is either a fit or an explicit error:
// - an error has a name and says what the input shows: a bad slice, quote or expiry order at a
//   slice index inside the input, every slice before it good and in order; a bad slice exactly when
//   that slice's F, D or T is not finite and positive; a bad quote at an index inside the slice
//   that is bad; too few strikes only with fewer than 5 distinct strikes; an expiry order error
//   only where T does not increase. No feasible start is named and has no input condition.
// - a fit has slices in order of expiry, one entry per slice and per quote, finite box coordinates
// inside the box, no
//   constraint violation, a finite non-negative Phi that is the sum of the slices' Phi_j, each the
//   sum of its squared residuals and stage 6b's Objective at the converted slice within 1e-9
//   relatively, plus a rounding scale; the starts are counted consistently.
// - the box point maps, through FromGlobal, to slices the checker passes.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include "svi/essvi.hpp"
#include "svi/essvi_fit.hpp"
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

constexpr std::size_t kHeaderBytes = 48;
constexpr std::size_t kSliceBytes = 24;
constexpr std::size_t kQuoteBytes = 24;
constexpr std::size_t kMaxQuotes = 12;
constexpr std::size_t kMaxSlices = 3;
constexpr double kRelative = 1e-9;

constexpr EssviFitOptions kOptions{.random_starts = 2, .max_iterations = 60};

double ReadF64(const std::uint8_t* data, std::size_t offset) {
  double value = 0.0;
  std::memcpy(&value, data + offset, sizeof value);
  return value;
}

double ReadFraction(const std::uint8_t* data, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, data + offset, sizeof value);
  return static_cast<double>(value) * 0x1p-32;
}

bool Named(std::string_view name) { return !name.empty() && name != "unknown"; }

bool Positive(double x) { return std::isfinite(x) && x > 0.0; }

bool GoodSlice(const FitSlice& s) {
  return Positive(s.forward) && Positive(s.discount) && Positive(s.expiry);
}

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

bool Close(double x, double y) {
  return std::fabs(x - y) <= kRelative * std::max(std::fabs(x), std::fabs(y));
}

void CheckSliceError(const FitSlice& slice, const FitError& e) {
  AADP_FUZZ_REQUIRE(Named(FitErrorName(e.code)), "slice error has a name");
  AADP_FUZZ_REQUIRE((e.code == FitErrorCode::kBadSlice) == !GoodSlice(slice),
                    "bad slice when it is");
  switch (e.code) {
    case FitErrorCode::kBadSlice:
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
    case FitErrorCode::kNoFeasibleStart:
      AADP_FUZZ_REQUIRE(false, "a slice error is never no feasible start");
      break;
  }
}

void CheckError(const std::vector<FitSlice>& slices, const EssviFitError& e) {
  AADP_FUZZ_REQUIRE(Named(EssviFitErrorName(e.code)), "error has a name");
  AADP_FUZZ_REQUIRE(e.code != EssviFitErrorCode::kSliceCount, "1 to 3 slices are a count");
  if (e.code == EssviFitErrorCode::kNoFeasibleStart) return;
  AADP_FUZZ_REQUIRE(e.slice < slices.size(), "error slice inside the input");
  for (std::size_t j = 0; j < e.slice; ++j) {
    AADP_FUZZ_REQUIRE(GoodSlice(slices[j]), "slices before the error are good");
    AADP_FUZZ_REQUIRE(j == 0 || slices[j].expiry > slices[j - 1].expiry,
                      "slices before the error are in order");
  }
  if (e.code == EssviFitErrorCode::kSlice) {
    CheckSliceError(slices[e.slice], e.fit);
  } else {
    AADP_FUZZ_REQUIRE(e.code == EssviFitErrorCode::kExpiryOrder, "known error code");
    AADP_FUZZ_REQUIRE(e.slice > 0 && GoodSlice(slices[e.slice]) &&
                          !(slices[e.slice].expiry > slices[e.slice - 1].expiry),
                      "expiry order error where T does not increase");
  }
}

void CheckFit(const std::vector<FitSlice>& slices, const EssviFitResult& r) {
  const std::size_t n = slices.size();
  AADP_FUZZ_REQUIRE(r.slices.size() == n, "one result per slice");
  for (std::size_t j = 1; j < n; ++j) {
    AADP_FUZZ_REQUIRE(slices[j].expiry > slices[j - 1].expiry, "a fit's slices are in order");
  }
  AADP_FUZZ_REQUIRE(r.x.size() == 3 * n && r.coordinates.size() == 3 * n, "3 n coordinates");
  for (std::size_t j = 0; j < 3 * n; ++j) {
    const EssviCoordinateState& c = r.coordinates[j];
    AADP_FUZZ_REQUIRE(std::isfinite(r.x[j]) && c.value == r.x[j], "coordinate finite");
    AADP_FUZZ_REQUIRE(c.lower <= c.value && c.value <= c.upper, "coordinate inside its box");
    AADP_FUZZ_REQUIRE(Named(EssviCoordinateName(c.coordinate)), "coordinate has a name");
  }
  AADP_FUZZ_REQUIRE(r.constraints.violations == 0, "no constraint violated");
  for (const EssviConstraintState& s : r.constraints.states) {
    AADP_FUZZ_REQUIRE(!s.violated, "no constraint state violated");
    AADP_FUZZ_REQUIRE(Named(EssviConstraintName(s.constraint)), "constraint has a name");
  }
  AADP_FUZZ_REQUIRE(std::isfinite(r.objective) && r.objective >= 0.0, "Phi finite");
  double total = 0.0;
  for (std::size_t j = 0; j < n; ++j) {
    const EssviSliceFit& s = r.slices[j];
    const std::size_t q = slices[j].quotes.size();
    AADP_FUZZ_REQUIRE(s.quotes == q && s.market_vol.size() == q && s.model_vol.size() == q &&
                          s.model_price.size() == q && s.residual.size() == q,
                      "per-quote sizes");
    double sum = 0.0;
    for (const double x : s.residual) sum += x * x;
    AADP_FUZZ_REQUIRE(Close(sum, s.objective), "Phi_j is the sum of its squared residuals");
    // Within 1e-9 relatively plus the rounding scale of tests/svi/essvi_fit_test.cpp: each residual
    // may move by d_i = 1024 eps V_i / sigma_i (docs/NOTES.md, "Stage 7c results").
    double rounding = 0.0;
    for (std::size_t i = 0; i < q; ++i) {
      const FitQuote& quote = slices[j].quotes[i];
      const double d = 1024.0 * 0x1p-52 * quote.price / quote.noise;
      rounding += d * ((2.0 * std::fabs(s.residual[i])) + d);
    }
    const double phi6b = Objective(slices[j], s.raw);
    AADP_FUZZ_REQUIRE(
        std::fabs(phi6b - s.objective) <= (kRelative * std::max(phi6b, s.objective)) + rounding,
        "Phi_j is stage 6b's Objective");
    AADP_FUZZ_REQUIRE(Named(PsiCapName(s.interval.cap)), "cap has a name");
    total += s.objective;
  }
  AADP_FUZZ_REQUIRE(Close(total, r.objective), "Phi is the sum of the Phi_j");
  AADP_FUZZ_REQUIRE(r.starts.size() == kOptions.random_starts + 1, "starts counted");
  AADP_FUZZ_REQUIRE(r.starts_at_best >= 1 && r.starts_at_best <= r.starts.size(), "starts at best");
  AADP_FUZZ_REQUIRE(Named(EssviStopName(r.stop)), "stop has a name");
  AADP_FUZZ_REQUIRE(r.converged == (r.stop != EssviStop::kIterations), "converged by its stop");
}

// The box of the global map, with the fit's margins: rho, theta_1 in [1e-6, 1], a_i in
// [0, 2 theta_1], c_1 in [kC1Lower, 1], c_i in [0, 1].
void CheckBoxPoint(const std::uint8_t* data, std::size_t n) {
  std::vector<double> x(3 * n);
  for (std::size_t i = 0; i < n; ++i) {
    x[i] = -kRhoBound + (2.0 * kRhoBound * ReadFraction(data, 12 + (4 * i)));
  }
  x[n] = std::pow(10.0, -6.0 + (6.0 * ReadFraction(data, 24)));
  for (std::size_t i = 1; i < n; ++i) x[n + i] = 2.0 * x[n] * ReadFraction(data, 24 + (4 * i));
  for (std::size_t i = 0; i < n; ++i) {
    const double lo = i == 0 ? kC1Lower : 0.0;
    x[(2 * n) + i] = lo + ((1.0 - lo) * ReadFraction(data, 36 + (4 * i)));
  }
  std::vector<PsiInterval> intervals;
  const std::vector<EssviSlice<double>> s = FromGlobal<double>(x, &intervals);
  AADP_FUZZ_REQUIRE(s.size() == n && intervals.size() == n, "a slice per box slice");
  AADP_FUZZ_REQUIRE(CheckEssviConstraints(s).violations == 0, "a box point is arbitrage-free");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < kHeaderBytes) return 0;
  const std::size_t wanted = (data[0] % kMaxSlices) + 1;
  CheckBoxPoint(data, wanted);
  std::vector<FitSlice> slices;
  std::size_t offset = kHeaderBytes;
  for (std::size_t j = 0; j < wanted && offset + kSliceBytes <= size; ++j) {
    const std::size_t room = (size - offset - kSliceBytes) / kQuoteBytes;
    const std::size_t n = std::min<std::size_t>(data[1 + j] % (kMaxQuotes + 1), room);
    const unsigned puts = data[4 + (2 * j)] | (static_cast<unsigned>(data[5 + (2 * j)]) << 8U);
    FitSlice slice{.forward = ReadF64(data, offset),
                   .discount = ReadF64(data, offset + 8),
                   .expiry = ReadF64(data, offset + 16),
                   .quotes = {}};
    offset += kSliceBytes;
    for (std::size_t i = 0; i < n; ++i, offset += kQuoteBytes) {
      slice.quotes.push_back(
          {.strike = ReadF64(data, offset),
           .price = ReadF64(data, offset + 8),
           .type = ((puts >> i) & 1U) != 0 ? OptionType::kPut : OptionType::kCall,
           .noise = ReadF64(data, offset + 16)});
    }
    slices.push_back(std::move(slice));
  }
  if (slices.empty()) return 0;
  const auto fit = FitEssviSurface(slices, kOptions);
  if (fit) {
    CheckFit(slices, *fit);
  } else {
    CheckError(slices, fit.error());
  }
  return 0;
}
