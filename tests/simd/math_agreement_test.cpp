// Check 1 of stage 5a: every lane kernel of src/simd/lane_math.hpp gives the bits of its scalar
// twin, on every lane, at every width this build can run. No tolerance: the target is bit-identical
// (src/math/common.hpp gives the conditions, lane_math.hpp how the lanes keep them).
//
// Inputs: the edges (zeros, infinities, NaNs of both signs and several payloads, subnormals, the
// overflow and underflow thresholds, every region boundary and its neighbours), arbitrary bit
// patterns, and dense samples of each kernel's domain. The edges are run at every lane offset, so
// each meets every lane and mixed neighbours.
#include <gtest/gtest.h>
#include <hwy/highway.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "lane_widths.hpp"
#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "pricer/normal_cdf.hpp"
#include "simd/lane_math.hpp"
#include "simd/lane_real.hpp"

#ifndef AADP_SMOKE_SIZE
#define AADP_SMOKE_SIZE 0
#endif

namespace aadp::simd {
namespace {

namespace hn = hwy::HWY_NAMESPACE;
using testing::ForEachWidth;
using testing::SameBits;

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261114;
constexpr std::size_t kRandomInputs = AADP_SMOKE_SIZE ? (1U << 12) : (1U << 20);

double FromBits(std::uint64_t bits) { return std::bit_cast<double>(bits); }

// Each value, its two neighbours, and its negative with neighbours.
void AddAround(std::vector<double>& out, double x) {
  for (const double v : {x, -x}) {
    out.push_back(v);
    out.push_back(std::nextafter(v, std::numeric_limits<double>::infinity()));
    out.push_back(std::nextafter(v, -std::numeric_limits<double>::infinity()));
  }
}

std::vector<double> Edges() {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  std::vector<double> out = {
      0.0,
      -0.0,
      kInf,
      -kInf,
      FromBits(0x7ff8000000000000U),  // the canonical quiet NaN
      FromBits(0xfff8000000000000U),  // with its sign set, as x86 makes them
      FromBits(0x7ff8000000000001U),  // payloads
      FromBits(0xfff80000deadbeefU),
      FromBits(0x7ff0000000000001U),  // signalling
      FromBits(0xfff7ffffffffffffU),
  };
  for (const double x : {0x1p-1074,
                         0x1p-1073,
                         0x1.fffffffffffffp-1023,
                         0x1p-1022,
                         0x1p-1000,
                         0x1p-53,
                         0x1p-52,
                         std::numeric_limits<double>::max(),
                         0.5,
                         1.0,
                         2.0,
                         std::numbers::sqrt2,
                         std::numbers::sqrt2 / 2.0,  // sqrt 1/2, exactly half
                         709.782712893384,
                         709.7827128933841,
                         710.0,
                         746.0,
                         745.1332191019412,
                         745.0,
                         708.0,
                         0.075,
                         0.925,
                         0.425,
                         1.3887943864964021e-11,  // AS241: exp(-25), where r = 5
                         0.46875,
                         4.0,
                         28.0,
                         0.6629126073623883,  // Phi: y = 0.46875 at x = -y sqrt 2
                         5.656854249492381,
                         39.59797974644666,  // y = 4 and y = 28
                         37.0,
                         38.5,
                         1e-300,
                         1e300,
                         0x1.5fe4df4c60464p-33,
                         0x1.4f72c68e4f73ep-619}) {
    AddAround(out, x);
  }
  for (const double p : {0.075, 0.925, 0.075 + 0x1p-60, 1.0 - 0x1p-53, 0x1p-53, 0.5 - 0.425,
                         0.5 + 0.425, 1.0 - 1.3887943864964021e-11}) {
    AddAround(out, p);
  }
  return out;
}

std::vector<double> RandomBits(std::mt19937_64& rng) {
  std::vector<double> out(kRandomInputs);
  for (double& x : out) x = FromBits(rng());
  return out;
}

std::vector<double> Uniform(std::mt19937_64& rng, double lo, double hi) {
  std::uniform_real_distribution<double> dist(lo, hi);
  std::vector<double> out(kRandomInputs);
  for (double& x : out) x = dist(rng);
  return out;
}

// Positive doubles with a uniform exponent, so every binade, subnormals included, is sampled.
std::vector<double> LogUniform(std::mt19937_64& rng) {
  std::vector<double> out(kRandomInputs);
  for (double& x : out) x = std::abs(FromBits(rng() & 0x7fefffffffffffffU));
  return out;
}

// Philox-grid uniforms, as the pricer draws them, and their tails.
std::vector<double> Probabilities(std::mt19937_64& rng) {
  std::vector<double> out(kRandomInputs);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const double u = (static_cast<double>(rng() >> 12) + 0.5) * 0x1p-52;
    out[i] = i % 4 == 0 ? std::ldexp(u, -static_cast<int>(rng() % 1000)) : u;
  }
  return out;
}

struct Agreement {
  std::size_t checked = 0;
  std::size_t mismatches = 0;
  // Bit patterns, so a NaN's sign and payload show.
  std::uint64_t first_input = 0;
  std::uint64_t first_scalar = 0;
  std::uint64_t first_lane = 0;
};

// Runs inputs through the lane kernel L at a time, at every offset 0..L-1, against the scalar.
template <class D, class LaneFn, class ScalarFn>
void Compare(D d, const std::vector<double>& inputs, const LaneFn& lane_fn,
             const ScalarFn& scalar_fn, Agreement& result) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  std::vector<double> in(kLanes);
  std::vector<double> out(kLanes);
  for (std::size_t offset = 0; offset < kLanes; ++offset) {
    for (std::size_t start = offset; start + kLanes <= inputs.size() + offset; start += kLanes) {
      for (std::size_t lane = 0; lane < kLanes; ++lane) {
        in[lane] = inputs[(start + lane) % inputs.size()];
      }
      hn::StoreU(lane_fn(d, hn::LoadU(d, in.data())), d, out.data());
      for (std::size_t lane = 0; lane < kLanes; ++lane) {
        const double expected = scalar_fn(in[lane]);
        ++result.checked;
        if (!SameBits(out[lane], expected) && result.mismatches++ == 0) {
          result.first_input = std::bit_cast<std::uint64_t>(in[lane]);
          result.first_scalar = std::bit_cast<std::uint64_t>(expected);
          result.first_lane = std::bit_cast<std::uint64_t>(out[lane]);
        }
      }
    }
    if (kLanes > 1 && offset == 0 && inputs.size() > 4096) break;  // offsets only for the edges
  }
}

template <class LaneFn, class ScalarFn>
void ExpectBitIdentical(const char* name, const std::vector<std::vector<double>>& sets,
                        const LaneFn& lane_fn, const ScalarFn& scalar_fn) {
  ForEachWidth([&](auto d) {
    constexpr std::size_t kLanes = hn::MaxLanes(decltype(d)());
    Agreement result;
    for (const auto& inputs : sets) Compare(d, inputs, lane_fn, scalar_fn, result);
    std::cout << "simd " << name << ": " << kLanes << " lanes, " << result.checked << " inputs, "
              << result.mismatches << " not bit-identical\n";
    EXPECT_EQ(result.mismatches, 0U)
        << name << " at " << kLanes << " lanes: first at x = 0x" << std::hex << result.first_input
        << ", scalar " << result.first_scalar << ", lane " << result.first_lane;
  });
}

TEST(SimdMath, ExpIsBitIdenticalOnEveryWidth) {
  std::mt19937_64 rng(kSeed);
  ExpectBitIdentical(
      "exp", {Edges(), RandomBits(rng), Uniform(rng, -760.0, 720.0), Uniform(rng, -1.0, 1.0)},
      [](auto d, auto v) { return vmath::Exp(d, v); }, [](double x) { return math::exp(x); });
}

TEST(SimdMath, LogIsBitIdenticalOnEveryWidth) {
  std::mt19937_64 rng(kSeed + 1);
  ExpectBitIdentical(
      "log", {Edges(), RandomBits(rng), LogUniform(rng), Uniform(rng, 0.5, 2.0)},
      [](auto d, auto v) { return vmath::Log(d, v); }, [](double x) { return math::log(x); });
}

TEST(SimdMath, SqrtIsBitIdenticalOnEveryWidth) {
  std::mt19937_64 rng(kSeed + 2);
  ExpectBitIdentical(
      "sqrt", {Edges(), RandomBits(rng), LogUniform(rng)},
      [](auto d, auto v) { return vmath::Sqrt(d, v); }, [](double x) { return math::sqrt(x); });
}

TEST(SimdMath, InverseNormalIsBitIdenticalOnEveryWidth) {
  std::mt19937_64 rng(kSeed + 3);
  ExpectBitIdentical(
      "inverse normal", {Edges(), RandomBits(rng), Probabilities(rng), Uniform(rng, -0.1, 1.1)},
      [](auto d, auto v) { return vmath::InverseNormal(d, v); },
      [](double p) { return math::inverse_normal(p); });
}

TEST(SimdMath, NormalCdfIsBitIdenticalOnEveryWidth) {
  std::mt19937_64 rng(kSeed + 4);
  ExpectBitIdentical(
      "normal cdf", {Edges(), RandomBits(rng), Uniform(rng, -45.0, 45.0), Uniform(rng, -1.0, 1.0)},
      [](auto d, auto v) { return vmath::NormalCdf(d, v); },
      [](double x) { return pricer::NormalCdf(x); });
}

// The comparisons a kernel may use keep the C++ predicates, NaN included, so a lane cannot take
// the other side of a Select from the scalar.
TEST(SimdMath, ComparisonsAreTheCppPredicates) {
  const std::vector<double> values = {
      std::numeric_limits<double>::quiet_NaN(), -1.0, -0.0, 0.0, 1.0,
      std::numeric_limits<double>::infinity()};
  ForEachWidth([&](auto d) {
    using D = decltype(d);
    constexpr std::size_t kLanes = hn::MaxLanes(D());
    for (const double a : values) {
      for (const double b : values) {
        const LaneReal<D> x(a);
        const LaneReal<D> y(b);
        const auto all = [d](auto mask) { return hn::CountTrue(d, mask) == kLanes; };
        const auto none = [d](auto mask) { return hn::CountTrue(d, mask) == 0; };
        const auto agrees = [&](auto mask, bool expected) {
          return expected ? all(mask) : none(mask);
        };
        EXPECT_TRUE(agrees(x < y, a < b)) << a << " < " << b;
        EXPECT_TRUE(agrees(x <= y, a <= b)) << a << " <= " << b;
        EXPECT_TRUE(agrees(x > y, a > b)) << a << " > " << b;
        EXPECT_TRUE(agrees(x >= y, a >= b)) << a << " >= " << b;
        EXPECT_TRUE(agrees(x == y, a == b)) << a << " == " << b;
        EXPECT_TRUE(agrees(x != y, a != b)) << a << " != " << b;
      }
    }
  });
}

}  // namespace
}  // namespace aadp::simd
