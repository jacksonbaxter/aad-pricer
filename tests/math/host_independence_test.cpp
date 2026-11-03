// Host independence: a hash of every output bit over a fixed input set, against a committed value.
//
// The inputs come from integer arithmetic and IEEE + - * / only, so every host builds the same set.
// The kernels use no libm call except IEEE sqrt, so every host must then produce the same hash.
// The expected values were recorded on the Mac (arm64, macOS). The same test on the Linux host
// (x86-64, clang and g++) is the proof that the two agree. A deliberate change to a kernel changes
// its hash: record the new value in the same commit, with the reason.
#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "math/exp.hpp"
#include "math/inverse_normal.hpp"
#include "math/log.hpp"
#include "math/sqrt.hpp"
#include "rng/philox.hpp"

namespace aadp::math {
namespace {

// Development seed, not 1729 or 271828.
constexpr std::uint64_t kSeed = 0x686173686d617468U;
constexpr std::size_t kCount = std::size_t{1} << 18;

std::uint64_t SplitMix64(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9e3779b97f4a7c15U);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9U;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebU;
  return z ^ (z >> 31);
}

double Uniform(std::uint64_t& state) {
  return static_cast<double>(SplitMix64(state) >> 11) * 0x1p-53;
}

// FNV-1a over the eight bytes of each output, low byte first, so byte order does not matter.
std::uint64_t Hash(const std::vector<double>& inputs, double (*kernel)(double)) {
  std::uint64_t hash = 0xcbf29ce484222325U;
  for (const double x : inputs) {
    const auto bits = std::bit_cast<std::uint64_t>(kernel(x));
    for (unsigned byte = 0; byte < 8U; ++byte) {
      hash = (hash ^ ((bits >> (8U * byte)) & 0xffU)) * 0x100000001b3U;
    }
  }
  return hash;
}

std::vector<double> Specials() {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  return {0.0,
          -0.0,
          1.0,
          -1.0,
          0.5,
          kInf,
          -kInf,
          std::numeric_limits<double>::quiet_NaN(),
          -std::numeric_limits<double>::quiet_NaN(),
          std::numeric_limits<double>::denorm_min(),
          -std::numeric_limits<double>::denorm_min(),
          std::numeric_limits<double>::min(),
          std::numeric_limits<double>::max(),
          -std::numeric_limits<double>::max(),
          709.78,
          -745.1,
          0.075,
          0.925};
}

std::vector<double> ExpInputs() {
  std::vector<double> inputs = Specials();
  std::uint64_t state = kSeed;
  for (std::size_t i = 0; i < kCount; ++i) inputs.push_back(-760.0 + 1480.0 * Uniform(state));
  for (std::size_t i = 0; i < kCount; ++i) inputs.push_back(-2.0 + 4.0 * Uniform(state));
  return inputs;
}

std::vector<double> PositiveInputs(std::uint64_t seed) {
  std::vector<double> inputs = Specials();
  std::uint64_t state = seed;
  for (std::size_t i = 0; i < kCount; ++i) {
    inputs.push_back(std::bit_cast<double>(SplitMix64(state) % 0x7ff0000000000000U));
  }
  for (std::size_t i = 0; i < kCount; ++i) inputs.push_back(0.25 + 3.75 * Uniform(state));
  return inputs;
}

std::vector<double> ProbabilityInputs() {
  std::vector<double> inputs = Specials();
  std::uint64_t state = kSeed + 2;
  for (std::size_t i = 0; i < kCount; ++i) inputs.push_back(UniformFromBits(SplitMix64(state)));
  for (std::size_t i = 0; i < kCount; ++i) inputs.push_back(Uniform(state));
  // Deep tails, from bit patterns below 0.075.
  for (std::size_t i = 0; i < kCount / 4; ++i) {
    inputs.push_back(std::bit_cast<double>(SplitMix64(state) % 0x3fb3333333333333U));
  }
  return inputs;
}

struct Expected {
  const char* kernel;
  std::uint64_t hash;
};

// Recorded on mac-m4max. Linux must reproduce these unchanged.
constexpr Expected kExp = {.kernel = "exp", .hash = 0xfb7a2b7cc3834f1eU};
constexpr Expected kLog = {.kernel = "log", .hash = 0xc248515d85cc9f0dU};
constexpr Expected kSqrt = {.kernel = "sqrt", .hash = 0xa570f34288c1f12aU};
constexpr Expected kInverseNormal = {.kernel = "inverse_normal", .hash = 0x3b727e7c05f6132aU};

void Check(const Expected& expected, const std::vector<double>& inputs, double (*kernel)(double)) {
  const std::uint64_t hash = Hash(inputs, kernel);
  std::printf("host-independence hash, %s, %zu inputs: 0x%016llx\n", expected.kernel, inputs.size(),
              static_cast<unsigned long long>(hash));
  EXPECT_EQ(hash, expected.hash) << expected.kernel << ": outputs differ from the committed hash";
}

TEST(MathHostIndependence, ExpHashIsCommitted) { Check(kExp, ExpInputs(), &math::exp); }
TEST(MathHostIndependence, LogHashIsCommitted) {
  Check(kLog, PositiveInputs(kSeed + 1), &math::log);
}
TEST(MathHostIndependence, SqrtHashIsCommitted) {
  Check(kSqrt, PositiveInputs(kSeed + 3), &math::sqrt);
}
TEST(MathHostIndependence, InverseNormalHashIsCommitted) {
  Check(kInverseNormal, ProbabilityInputs(), &math::inverse_normal);
}

}  // namespace
}  // namespace aadp::math
