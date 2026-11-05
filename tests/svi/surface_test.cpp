// The surface of svi/surface.hpp against NOTES.md, "Stage 7a": validation, where a time sits,
// interpolation linear in T at fixed k, (t / T_0) w_0 before the first slice, the slices
// themselves at their own times, and the forward and discount curves. Then the same on the tape:
// its values must be the double's bits.
//
// Tolerances, stated before the checks ran. Where the surface must reproduce a slice at its own
// time, or a curve its node, the claim is exact: the formulas make it so (weight 0 or 1, the node
// returned as stored). Interpolated values are compared with raw_svi.hpp's independent formulas
// within 16 eps of the sum of the magnitudes involved: each side is a handful of roundings.
#include "svi/surface.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "ad/real.hpp"  // IWYU pragma: keep (the tape's operators, by ADL)
#include "ad/tape.hpp"
#include "math/log.hpp"
#include "svi/raw_svi.hpp"
#include "svi/synthetic_surface.hpp"

namespace aadp::svi {
namespace {

using testing::Surface;

constexpr double kEps = 0x1p-52;

std::uint64_t Bits(double x) { return std::bit_cast<std::uint64_t>(x); }

SviSurface<double> Build(const Surface& s) { return {s.terms, s.params}; }

TEST(Surface, ValidationNamesEveryError) {
  const Surface good = testing::SurfaceA();
  EXPECT_EQ(ValidateSurface(good.terms, good.params), SurfaceError::kNone);
  EXPECT_EQ(ValidateSurface({}, {}), SurfaceError::kSliceCount);
  EXPECT_EQ(ValidateSurface(std::span(good.terms).first(2), good.params),
            SurfaceError::kSliceCount);

  const auto with_terms = [&](std::size_t j, SliceTerms t) {
    Surface s = good;
    s.terms[j] = t;
    return ValidateSurface(s.terms, s.params);
  };
  const SliceTerms t1 = good.terms[1];
  EXPECT_EQ(
      with_terms(1,
                 {.expiry = good.terms[0].expiry, .forward = t1.forward, .discount = t1.discount}),
      SurfaceError::kExpiry);
  EXPECT_EQ(with_terms(1, {.expiry = good.terms[0].expiry + 1e-7,
                           .forward = t1.forward,
                           .discount = t1.discount}),
            SurfaceError::kExpiry);
  EXPECT_EQ(with_terms(0, {.expiry = 0.0, .forward = 100.0, .discount = 1.0}),
            SurfaceError::kExpiry);
  EXPECT_EQ(with_terms(1, {.expiry = t1.expiry, .forward = std::nan(""), .discount = 1.0}),
            SurfaceError::kForward);
  EXPECT_EQ(with_terms(1, {.expiry = t1.expiry, .forward = 100.0, .discount = -1.0}),
            SurfaceError::kDiscount);

  const auto with_params = [&](RawSvi<double> p) {
    Surface s = good;
    s.params[2] = p;
    return ValidateSurface(s.terms, s.params);
  };
  const RawSvi<double> p = good.params[2];
  EXPECT_EQ(with_params({.a = p.a, .b = -1e-9, .rho = p.rho, .m = p.m, .sigma = p.sigma}),
            SurfaceError::kParameters);
  EXPECT_EQ(with_params({.a = p.a, .b = p.b, .rho = 1.0 + 1e-12, .m = p.m, .sigma = p.sigma}),
            SurfaceError::kParameters);
  EXPECT_EQ(with_params({.a = p.a, .b = p.b, .rho = p.rho, .m = p.m, .sigma = 1e-9}),
            SurfaceError::kParameters);
  EXPECT_EQ(with_params({.a = std::numeric_limits<double>::infinity(),
                         .b = p.b,
                         .rho = p.rho,
                         .m = p.m,
                         .sigma = p.sigma}),
            SurfaceError::kParameters);
  // b = 0 and rho = +-1 are inside the domain.
  EXPECT_EQ(with_params({.a = p.a, .b = 0.0, .rho = -1.0, .m = p.m, .sigma = p.sigma}),
            SurfaceError::kNone);
}

TEST(Surface, LocateGivesTheSegmentAndItsWeight) {
  const std::array<double, 3> t = testing::kExpiries;
  const TimeSlot zero = Locate(t, 0.0);
  EXPECT_EQ(zero.segment, 0U);
  EXPECT_EQ(zero.weight, 0.0);
  EXPECT_EQ(zero.inverse_length, 1.0 / t[0]);
  const TimeSlot early = Locate(t, 0.25 * t[0]);
  EXPECT_EQ(early.segment, 0U);
  EXPECT_EQ(early.weight, (0.25 * t[0]) / t[0]);
  // A slice's own time starts the next segment, with weight 0.
  const TimeSlot first = Locate(t, t[0]);
  EXPECT_EQ(first.segment, 1U);
  EXPECT_EQ(first.weight, 0.0);
  EXPECT_EQ(first.inverse_length, 1.0 / (t[1] - t[0]));
  const TimeSlot second = Locate(t, t[1]);
  EXPECT_EQ(second.segment, 2U);
  EXPECT_EQ(second.weight, 0.0);
  EXPECT_EQ(second.inverse_length, 1.0 / (t[2] - t[1]));
  const double mid = t[1] + (0.75 * (t[2] - t[1]));
  const TimeSlot late = Locate(t, mid);
  EXPECT_EQ(late.segment, 2U);
  EXPECT_EQ(late.weight, (mid - t[1]) / (t[2] - t[1]));
  // The last slice's time ends the last segment, with weight 1.
  const TimeSlot last = Locate(t, t[2]);
  EXPECT_EQ(last.segment, 2U);
  EXPECT_EQ(last.weight, 1.0);
  // One slice: everything is segment 0, up to and including its time.
  const std::array<double, 1> one = {0.5};
  EXPECT_EQ(Locate(one, 0.5).segment, 0U);
  EXPECT_EQ(Locate(one, 0.5).weight, 1.0);
}

double Magnitude(const RawSvi<double>& p, double k) {
  const double x = k - p.m;
  return std::fabs(p.a) + (p.b * (std::fabs(p.rho * x) + std::sqrt((x * x) + (p.sigma * p.sigma))));
}

TEST(Surface, EachSliceComesBackExactlyAtItsOwnTime) {
  for (const Surface& s : {testing::SurfaceA(), testing::SurfaceB()}) {
    const SviSurface<double> surface = Build(s);
    for (std::size_t j = 0; j < 3; ++j) {
      const TimeSlot slot = surface.Locate(s.terms[j].expiry);
      for (int i = -40; i <= 40; ++i) {
        const double k = 0.01 * i;
        const SviPoint<double> want = EvaluateSlice(surface.Slice(j), k);
        const SurfacePoint<double> got = surface.At(slot, k);
        ASSERT_EQ(Bits(got.w), Bits(want.w)) << "slice " << j << ", k " << k;
        ASSERT_EQ(Bits(got.w1), Bits(want.w1)) << "slice " << j << ", k " << k;
        ASSERT_EQ(Bits(got.w2), Bits(want.w2)) << "slice " << j << ", k " << k;
      }
    }
  }
}

TEST(Surface, TotalVarianceIsLinearInTimeAtFixedKAndScaledBeforeTheFirstSlice) {
  for (const Surface& s : {testing::SurfaceA(), testing::SurfaceB()}) {
    const SviSurface<double> surface = Build(s);
    for (const double fraction : {0.0, 0.1, 0.5, 0.9}) {
      for (std::size_t segment = 0; segment < 3; ++segment) {
        const double lo = segment == 0 ? 0.0 : s.terms[segment - 1].expiry;
        const double hi = s.terms[segment].expiry;
        const double t = lo + (fraction * (hi - lo));
        const TimeSlot slot = surface.Locate(t);
        ASSERT_EQ(slot.segment, segment);
        for (int i = -40; i <= 40; ++i) {
          const double k = 0.01 * i;
          const SurfacePoint<double> got = surface.At(slot, k);
          const RawSvi<double>& b = s.params[segment];
          const double wb = TotalVariance(b, k);
          const SviPoint<double> eb = Evaluate(b, k);
          double want_w = 0.0;
          double want_w1 = 0.0;
          double want_dwdt = 0.0;
          double scale = 0.0;
          if (segment == 0) {
            want_w = (t / hi) * wb;
            want_w1 = (t / hi) * eb.w1;
            want_dwdt = wb / hi;
            scale = Magnitude(b, k);
            EXPECT_EQ(Bits(got.q_den), Bits(EvaluateSlice(surface.Slice(0), k).w));
          } else {
            const RawSvi<double>& a = s.params[segment - 1];
            const double alpha = (t - lo) / (hi - lo);
            want_w = ((1.0 - alpha) * TotalVariance(a, k)) + (alpha * wb);
            want_w1 = ((1.0 - alpha) * Evaluate(a, k).w1) + (alpha * eb.w1);
            want_dwdt = (wb - TotalVariance(a, k)) / (hi - lo);
            scale = Magnitude(a, k) + Magnitude(b, k);
            EXPECT_EQ(Bits(got.q_den), Bits(got.w));
          }
          const double tol = 16.0 * kEps * scale;
          ASSERT_LE(std::fabs(got.w - want_w), tol) << "segment " << segment << ", k " << k;
          ASSERT_LE(std::fabs(got.dwdt - want_dwdt), tol / (hi - lo))
              << "segment " << segment << ", k " << k;
          ASSERT_LE(std::fabs(got.w1 - want_w1), 16.0 * kEps * (std::fabs(want_w1) + 4.0))
              << "segment " << segment << ", k " << k;
        }
      }
    }
  }
}

TEST(Surface, CurvesPassThroughTheirNodesWithPiecewiseConstantCarryAndRate) {
  const Surface s = testing::SurfaceA();
  const ForwardCurve curve(s.terms);
  EXPECT_EQ(Bits(curve.Spot()), Bits(s.terms[0].discount * s.terms[0].forward));
  EXPECT_EQ(Bits(curve.LogForward(0.0)), Bits(math::log(curve.Spot())));
  EXPECT_EQ(curve.LogDiscount(0.0), 0.0);
  EXPECT_EQ(curve.LastExpiry(), s.terms[2].expiry);
  for (const SliceTerms& t : s.terms) {
    EXPECT_EQ(Bits(curve.LogForward(t.expiry)), Bits(math::log(t.forward)));
    EXPECT_EQ(Bits(curve.LogDiscount(t.expiry)), Bits(math::log(t.discount)));
    EXPECT_LE(std::fabs(curve.Discount(t.expiry) / t.discount - 1.0), 4.0 * kEps);
  }
  // Inside each segment the slope of ln F and of ln D is the segment's own, so carry and rate
  // are constant there.
  std::array<double, 4> times = {0.0, s.terms[0].expiry, s.terms[1].expiry, s.terms[2].expiry};
  std::array<double, 4> log_f = {math::log(curve.Spot()), math::log(s.terms[0].forward),
                                 math::log(s.terms[1].forward), math::log(s.terms[2].forward)};
  std::array<double, 4> log_d = {0.0, math::log(s.terms[0].discount),
                                 math::log(s.terms[1].discount), math::log(s.terms[2].discount)};
  for (std::size_t i = 0; i + 1 < times.size(); ++i) {
    const double length = times[i + 1] - times[i];
    const double carry = (log_f[i + 1] - log_f[i]) / length;
    const double rate = (log_d[i + 1] - log_d[i]) / length;
    for (const double fraction : {0.2, 0.5, 0.8}) {
      const double t = times[i] + (fraction * length);
      const double f = log_f[i] + (carry * (t - times[i]));
      const double d = log_d[i] + (rate * (t - times[i]));
      EXPECT_LE(std::fabs(curve.LogForward(t) - f), 16.0 * kEps * std::fabs(f)) << i;
      EXPECT_LE(std::fabs(curve.LogDiscount(t) - d), 16.0 * kEps * (std::fabs(d) + 1e-3)) << i;
    }
  }
}

TEST(Surface, TheTapeRecordsTheDoubleValuesBitForBit) {
  const Surface s = testing::SurfaceB();
  const SviSurface<double> plain = Build(s);
  ad::Tape tape;
  const ad::TapeScope scope(tape);
  std::vector<RawSvi<ad::Real>> active;
  active.reserve(s.params.size());
  for (const RawSvi<double>& p : s.params) {
    active.push_back({.a = tape.Register(p.a),
                      .b = tape.Register(p.b),
                      .rho = tape.Register(p.rho),
                      .m = tape.Register(p.m),
                      .sigma = tape.Register(p.sigma)});
  }
  const SviSurface<ad::Real> surface(s.terms, active);
  for (const double t : {0.0, 0.004, 0.02, 0.05, s.terms[2].expiry}) {
    const TimeSlot slot = plain.Locate(t);
    for (int i = -6; i <= 6; ++i) {
      const double k = 0.05 * i;
      const SurfacePoint<double> want = plain.At(slot, k);
      const SurfacePoint<ad::Real> got = surface.At(slot, ad::Real(k));
      ASSERT_EQ(Bits(got.w.value()), Bits(want.w));
      ASSERT_EQ(Bits(got.w1.value()), Bits(want.w1));
      ASSERT_EQ(Bits(got.w2.value()), Bits(want.w2));
      ASSERT_EQ(Bits(got.dwdt.value()), Bits(want.dwdt));
      ASSERT_EQ(Bits(got.q_den.value()), Bits(want.q_den));
    }
  }
}

}  // namespace
}  // namespace aadp::svi
