// The svi area on raw bytes: Black-76 inversion, the butterfly checks and the calendar check, with
// every field free to take any value, NaN and infinities included:
//
//   offset  0  u8       mode % 3: 0 inversion, 1 butterfly, 2 calendar
//   offset  1  u8       option type (low bit: 0 call, 1 put)
//   offset  2  u8 x 6   unused
//   offset  8  f64 x 10 inversion: forward, discount, strike, expiry, price, vol;
//                       butterfly: a, b, rho, m, sigma; calendar: a, b, rho, m, sigma of the
//                       earlier slice, then of the later
//   88 bytes, little-endian. A shorter input is not a case.
//
// Properties. Nothing throws, crashes or trips a sanitizer, and every output is either valid or
// an explicit error:
// - inversion: an error status has a name and vol NaN, at-intrinsic has vol 0, ok has a finite
//   positive vol that prices back inside the discounted bounds, and within 1000 times SVI-1's
//   price tolerance (NOTES.md, stage 6a) of the quoted price; a non-finite input is reported as
//   such. The price from the vol field goes through the same inversion.
// - checks: every violation has a known kind and the fields that kind promises (a grid region
//   is finite and ordered, with its worst point inside and a value below its bound; a wing is at
//   +-infinity; a classifier failure is type 0 to 4); invalid parameters produce only that
//   report, and the classifier agrees with the parameter check.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "svi/arbitrage.hpp"
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

constexpr std::size_t kInputBytes = 88;
// Coarser than the default, so an input runs in about a millisecond; the logic is the same.
constexpr GridSpec kSpec{.k_half_width = 3.0, .k_step = 1e-2, .y_half_width = 40.0, .y_step = 5e-2};

double ReadF64(const std::uint8_t* data, std::size_t index) {
  double value = 0.0;
  std::memcpy(&value, data + 8 + (8 * index), sizeof value);
  return value;
}

SviParams ReadSlice(const std::uint8_t* data, std::size_t first) {
  return {.a = ReadF64(data, first),
          .b = ReadF64(data, first + 1),
          .rho = ReadF64(data, first + 2),
          .m = ReadF64(data, first + 3),
          .sigma = ReadF64(data, first + 4)};
}

bool Named(std::string_view name) { return !name.empty() && name != "unknown"; }

void CheckInversion(const Black76Quote& q) {
  const ImpliedVol r = ImpliedVolatility(q);
  AADP_FUZZ_REQUIRE(Named(StatusName(r.status)), "status has a name");
  const bool finite = std::isfinite(q.forward) && std::isfinite(q.discount) &&
                      std::isfinite(q.strike) && std::isfinite(q.expiry) && std::isfinite(q.price);
  AADP_FUZZ_REQUIRE(finite || r.status == InversionStatus::kNonFiniteInput, "non-finite named");
  if (r.status == InversionStatus::kAtIntrinsic) {
    AADP_FUZZ_REQUIRE(r.vol == 0.0, "at-intrinsic vol is 0");
    return;
  }
  if (r.status != InversionStatus::kOk) {
    AADP_FUZZ_REQUIRE(std::isnan(r.vol), "error vol is NaN");
    return;
  }
  AADP_FUZZ_REQUIRE(std::isfinite(r.vol) && r.vol > 0.0, "ok vol finite and positive");
  const double back = Black76Price(q.forward, q.discount, q.strike, q.expiry, r.vol, q.type);
  AADP_FUZZ_REQUIRE(std::isfinite(back) && back >= 0.0, "price of the vol finite");
  const bool call = q.type == OptionType::kCall;
  const double maximum = q.discount * (call ? q.forward : q.strike);
  AADP_FUZZ_REQUIRE(back <= maximum * (1.0 + 0x1p-50), "price of the vol below the maximum");
  // SVI-1's price tolerance, 8 eta (1 + 1/C), widened 1000 times: the fuzzer's inputs reach far
  // beyond the stated grid, and this property is about gross failures, the grid about accuracy.
  const double c = VolConditionNumber(q.forward, q.strike, q.expiry, r.vol, q.type);
  if (std::isfinite(c) && c > 0.0 && q.price > 0.0) {
    // The subnormal spacing counts at the smallest of the price, the undiscounted price V / D and
    // Let's Be Rational's normalised price V / (D sqrt(F K)): on the grid D <= 1 and the last is
    // normal, but here D, F and K may be anything.
    const double undiscounted = q.price / q.discount;
    const double normalised = undiscounted / (std::sqrt(q.forward) * std::sqrt(q.strike));
    const double smallest = std::fmin(q.price, std::fmin(undiscounted, normalised));
    const double eta = 0x1p-52 + (0x1p-1074 / smallest);
    const double tolerance = 8000.0 * eta * (1.0 + (1.0 / c));
    if (!(std::fabs((back / q.price) - 1.0) <= tolerance)) {
      std::fprintf(stderr, "price %.17g back %.17g vol %.17g C %.17g tolerance %.17g\n", q.price,
                   back, r.vol, c, tolerance);
    }
    AADP_FUZZ_REQUIRE(std::fabs((back / q.price) - 1.0) <= tolerance, "price round trip");
  }
}

void CheckViolation(const Violation& v) {
  AADP_FUZZ_REQUIRE(v.kind <= ViolationKind::kCalendarLeftWing, "known kind");
  AADP_FUZZ_REQUIRE(Named(KindName(v.kind)), "kind has a name");
  switch (v.kind) {
    case ViolationKind::kDensity:
    case ViolationKind::kCalendarCrossing:
      AADP_FUZZ_REQUIRE(v.points >= 1, "region has points");
      AADP_FUZZ_REQUIRE(std::isfinite(v.k_begin) && std::isfinite(v.k_end), "region finite");
      AADP_FUZZ_REQUIRE(v.k_begin <= v.k_worst && v.k_worst <= v.k_end, "worst inside");
      AADP_FUZZ_REQUIRE(std::isfinite(v.value) && v.value < v.bound && v.bound <= 0.0,
                        "value below bound");
      break;
    case ViolationKind::kNonFiniteDensity:
      AADP_FUZZ_REQUIRE(v.points == 1 && std::isfinite(v.k_worst), "non-finite at a point");
      AADP_FUZZ_REQUIRE(!std::isfinite(v.value), "non-finite value");
      break;
    case ViolationKind::kDPlusLimit:
    case ViolationKind::kRightWing:
    case ViolationKind::kCalendarRightWing:
      AADP_FUZZ_REQUIRE(v.points == 0 && v.k_worst > 0.0 && std::isinf(v.k_worst), "at +inf");
      AADP_FUZZ_REQUIRE(std::isfinite(v.value), "wing value finite");
      break;
    case ViolationKind::kLeftWing:
    case ViolationKind::kCalendarLeftWing:
      AADP_FUZZ_REQUIRE(v.points == 0 && v.k_worst < 0.0 && std::isinf(v.k_worst), "at -inf");
      AADP_FUZZ_REQUIRE(std::isfinite(v.value), "wing value finite");
      break;
    case ViolationKind::kMartiniMingone:
      AADP_FUZZ_REQUIRE(
          v.value == 0.0 || v.value == 1.0 || v.value == 2.0 || v.value == 3.0 || v.value == 4.0,
          "classifier type 0 to 4");
      break;
    case ViolationKind::kInvalidParameters:
      AADP_FUZZ_REQUIRE(std::isnan(v.k_worst), "invalid has no location");
      break;
    case ViolationKind::kNonPositiveVariance:
      AADP_FUZZ_REQUIRE(!(v.value > 0.0), "non-positive variance value");
      break;
  }
}

void CheckSlice(const SviParams& p) {
  const std::vector<Violation> parameters = CheckParameters(p);
  const ButterflyReport report = CheckButterfly(p, kSpec);
  const MmVerdict verdict = report.classifier.verdict;
  AADP_FUZZ_REQUIRE(verdict <= MmVerdict::kUndecided && Named(MmVerdictName(verdict)),
                    "verdict has a name");
  for (const Violation& v : report.violations) CheckViolation(v);
  if (!parameters.empty()) {
    AADP_FUZZ_REQUIRE(report.violations.size() == parameters.size(), "only parameter report");
    AADP_FUZZ_REQUIRE(report.grid_points == 0, "invalid slice not evaluated");
    AADP_FUZZ_REQUIRE(
        verdict == MmVerdict::kInvalidParameters || verdict == MmVerdict::kNonPositiveVariance,
        "classifier agrees on parameters");
    return;
  }
  AADP_FUZZ_REQUIRE(
      verdict != MmVerdict::kInvalidParameters && verdict != MmVerdict::kNonPositiveVariance,
      "classifier agrees on valid parameters");
  AADP_FUZZ_REQUIRE(report.grid_points > 0, "valid slice evaluated");
  const bool type1 = p.b * (1.0 + p.rho) > 2.0 || p.b * (1.0 - p.rho) > 2.0;
  AADP_FUZZ_REQUIRE(type1 == (verdict == MmVerdict::kType1), "type 1 is the wing slopes");
  const double tolerance = DensityTolerance(p, p.m);
  AADP_FUZZ_REQUIRE(!(tolerance < 0.0), "density tolerance not negative");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < kInputBytes) return 0;
  const OptionType type = (data[1] & 1U) == 0 ? OptionType::kCall : OptionType::kPut;
  switch (data[0] % 3) {
    case 0: {
      Black76Quote q{.forward = ReadF64(data, 0),
                     .discount = ReadF64(data, 1),
                     .strike = ReadF64(data, 2),
                     .expiry = ReadF64(data, 3),
                     .price = ReadF64(data, 4),
                     .type = type};
      CheckInversion(q);
      q.price = Black76Price(q.forward, q.discount, q.strike, q.expiry, ReadF64(data, 5), type);
      CheckInversion(q);
      break;
    }
    case 1:
      CheckSlice(ReadSlice(data, 0));
      break;
    default: {
      const SviParams earlier = ReadSlice(data, 0);
      const SviParams later = ReadSlice(data, 5);
      const CalendarReport report = CheckCalendar(earlier, later, kSpec);
      for (const Violation& v : report.violations) CheckViolation(v);
      const bool valid = CheckParameters(earlier).empty() && CheckParameters(later).empty();
      AADP_FUZZ_REQUIRE(valid == (report.grid_points > 0), "valid pairs evaluated");
      for (const Violation& v : CheckCalendarWings(earlier, later)) {
        if (valid) CheckViolation(v);
      }
      break;
    }
  }
  return 0;
}
