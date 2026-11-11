#include "metal/probes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "rng/philox.hpp"

namespace aadp::metal::probes {

namespace {

std::uint32_t FloatBits(float x) { return std::bit_cast<std::uint32_t>(x); }
float FromBits(std::uint32_t bits) { return std::bit_cast<float>(bits); }

// Sign and mantissa from two words, the unbiased exponent uniform on [-spread, spread] from a
// third.
std::uint32_t RandomFloatBits(std::uint64_t seed, std::uint64_t k, std::uint32_t tag,
                              std::uint32_t spread, std::int32_t offset) {
  const std::uint32_t sign = RandomWord(seed, k, tag) & 0x80000000U;
  const std::uint32_t mantissa = RandomWord(seed, k, tag + 1) & 0x007FFFFFU;
  const auto exponent = static_cast<std::int32_t>(RandomWord(seed, k, tag + 2) % (2 * spread + 1)) -
                        static_cast<std::int32_t>(spread) + offset;
  return sign | (static_cast<std::uint32_t>(exponent + 127) << 23U) | mantissa;
}

struct Constructed {
  const char* name;
  float a;
  float b;
};

// Ties and near-ties: under round to nearest even and under round toward zero these give
// different results, so they say which mode the GPU uses.
constexpr std::array<Constructed, 11> kConstructed = {{
    {.name = "1 + 2^-24 (a tie, even is 1)", .a = 1.0F, .b = 0x1p-24F},
    {.name = "1 + 3 2^-24 (a tie, even is 1 + 2^-22)", .a = 1.0F, .b = 0x3p-24F},
    {.name = "1 + (2^-24 + 2^-30) (above the midpoint)", .a = 1.0F, .b = 0x1.04p-24F},
    {.name = "1 + (2^-24 - 2^-30) (below the midpoint)", .a = 1.0F, .b = 0x1.f8p-25F},
    {.name = "-1 - 3 2^-24 (a tie, negative)", .a = -1.0F, .b = -0x3p-24F},
    {.name = "2^24 + 1 (a tie, even is 2^24)", .a = 0x1p24F, .b = 1.0F},
    {.name = "2^24 + 3 (a tie, even is 2^24 + 4)", .a = 0x1p24F, .b = 3.0F},
    {.name = "1 - 2^-25 (a tie below 1, even is 1)", .a = 1.0F, .b = -0x1p-25F},
    {.name = "(1 + 2^-23) and (1 - 2^-23): product 1 - 2^-46",
     .a = 1.0F + 0x1p-23F,
     .b = 1.0F - 0x1p-23F},
    {.name = "1 and 3: quotient 1/3", .a = 1.0F, .b = 3.0F},
    {.name = "2 and 3: quotient 2/3", .a = 2.0F, .b = 3.0F},
}};

}  // namespace

std::uint32_t RandomWord(std::uint64_t seed, std::uint64_t k, std::uint32_t tag) {
  return Philox4x32_10(
      {static_cast<std::uint32_t>(k), static_cast<std::uint32_t>(k >> 32U), tag, 0U},
      {static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32U)})[0];
}

std::vector<std::uint32_t> ArithmeticInputs(std::uint64_t seed, std::uint32_t random_pairs) {
  std::vector<std::uint32_t> out;
  out.reserve(2 * (random_pairs + kConstructed.size()));
  for (std::uint32_t i = 0; i < random_pairs; ++i) {
    out.push_back(RandomFloatBits(seed, i, 0, 30, 0));
    out.push_back(RandomFloatBits(seed, i, 3, 30, 0));
  }
  for (const Constructed& c : kConstructed) {
    out.push_back(FloatBits(c.a));
    out.push_back(FloatBits(c.b));
  }
  return out;
}

std::vector<std::string> ConstructedNames() {
  std::vector<std::string> out;
  out.reserve(kConstructed.size());
  for (const Constructed& c : kConstructed) out.emplace_back(c.name);
  return out;
}

std::vector<std::uint32_t> ArithmeticReference(std::span<const std::uint32_t> pairs) {
  std::vector<std::uint32_t> out;
  out.reserve(pairs.size() / 2 * 5);
  for (std::size_t i = 0; i + 1 < pairs.size(); i += 2) {
    const float a = FromBits(pairs[i]);
    const float b = FromBits(pairs[i + 1]);
    out.push_back(FloatBits(a + b));
    out.push_back(FloatBits(a - b));
    out.push_back(FloatBits(a * b));
    out.push_back(FloatBits(a / b));
    out.push_back(FloatBits(std::sqrt(std::fabs(a))));
  }
  return out;
}

std::vector<std::uint32_t> ContractionInputs(std::uint64_t seed, std::uint32_t triples) {
  std::vector<std::uint32_t> out;
  out.reserve(3 * static_cast<std::size_t>(triples));
  for (std::uint32_t i = 0; i < triples; ++i) {
    const std::uint32_t a = RandomFloatBits(seed, i, 10, 15, 0);
    const std::uint32_t b = RandomFloatBits(seed, i, 13, 15, 0);
    // c near the product's size, so the product's rounding reaches the sum's.
    const auto product_exponent = static_cast<std::int32_t>((a >> 23U) & 0xFFU) +
                                  static_cast<std::int32_t>((b >> 23U) & 0xFFU) - 254;
    out.push_back(a);
    out.push_back(b);
    out.push_back(RandomFloatBits(seed, i, 16, 1, product_exponent));
  }
  return out;
}

std::vector<std::uint32_t> ContractionReference(std::span<const std::uint32_t> triples) {
  std::vector<std::uint32_t> out;
  out.reserve(triples.size() / 3);
  for (std::size_t i = 0; i + 2 < triples.size(); i += 3) {
    const float product = FromBits(triples[i]) * FromBits(triples[i + 1]);
    out.push_back(FloatBits(product + FromBits(triples[i + 2])));
  }
  return out;
}

std::vector<std::uint32_t> UintInputs(std::uint64_t seed, std::uint32_t random_words) {
  std::vector<std::uint32_t> out = {(1U << 24U) + 1U, (1U << 24U) + 3U, 0xFFFFFF7FU, 0xFFFFFF80U,
                                    0xFFFFFFFFU};
  for (std::uint32_t i = 0; i < random_words; ++i) out.push_back(RandomWord(seed, i, 20));
  return out;
}

std::vector<std::uint32_t> DenormalInputs() {
  constexpr std::array<std::uint32_t, 4> kDenormals = {0x00000001U, 0x00000200U, 0x00400000U,
                                                       0x007FFFFFU};
  std::vector<std::uint32_t> out;
  for (const std::uint32_t x : kDenormals) {
    out.insert(out.end(), {x, FloatBits(1.0F), x, FloatBits(0.0F)});
  }
  for (const float k : {0x1p-1F, 0x1p-10F, 0x1p-23F}) {
    out.insert(out.end(), {FloatBits(0x1p-126F), FloatBits(k)});
  }
  out.insert(out.end(), {FloatBits(0x1.8p-126F), FloatBits(-0x1p-126F)});
  return out;
}

std::vector<std::string> DenormalNames() {
  return {"x = 2^-149, b = 1",
          "x = 2^-149, b = 0",
          "x = 2^-140, b = 1",
          "x = 2^-140, b = 0",
          "x = 2^-127, b = 1",
          "x = 2^-127, b = 0",
          "x = largest denormal, b = 1",
          "x = largest denormal, b = 0",
          "2^-126 * 2^-1",
          "2^-126 * 2^-10",
          "2^-126 * 2^-23",
          "1.5 2^-126 + (-2^-126)"};
}

std::vector<float> GridPoints(Grid grid) {
  constexpr std::uint32_t kSteps = 1U << 22U;
  std::vector<float> out;
  out.reserve(kSteps + 1);
  for (std::uint32_t i = 0; i <= kSteps; ++i) {
    const double f = static_cast<double>(i) / static_cast<double>(kSteps);
    double x = 0.0;
    switch (grid) {
      case Grid::kExpWide:
        x = -87.0 + 175.0 * f;
        break;
      case Grid::kExpPricer:
        x = -3.0 + 6.0 * f;
        break;
      case Grid::kLogWide:
        x = std::exp2(-126.0 + 254.0 * f);
        break;
      case Grid::kLogUnit:
        x = 0.5 + 1.5 * f;
        break;
    }
    const auto point = static_cast<float>(x);
    // float(2^128) is +inf: the wide log grid's last point is left out, as NOTES.md records.
    if (std::isfinite(point)) out.push_back(point);
  }
  return out;
}

const char* GridName(Grid grid) {
  switch (grid) {
    case Grid::kExpWide:
      return "exp, x = float(-87 + 175 i / 2^22)";
    case Grid::kExpPricer:
      return "exp, x = float(-3 + 6 i / 2^22)";
    case Grid::kLogWide:
      return "log, x = float(2^t), t = -126 + 254 i / 2^22";
    case Grid::kLogUnit:
      return "log, x = float(0.5 + 1.5 i / 2^22)";
  }
  return "?";
}

double UlpError(float got, double exact) {
  if (!std::isfinite(got)) return std::numeric_limits<double>::infinity();
  const double magnitude = std::fabs(exact);
  double ulp = 0x1p-149;
  if (magnitude >= static_cast<double>(std::numeric_limits<float>::min())) {
    int exponent = 0;
    std::frexp(magnitude, &exponent);
    ulp = std::ldexp(1.0, exponent - 24);
  }
  return std::fabs(static_cast<double>(got) - exact) / ulp;
}

namespace {

template <class Exact>
FunctionError Measure(std::span<const float> x, std::span<const float> got, Exact exact) {
  FunctionError e;
  double total = 0.0;
  for (std::size_t i = 0; i < x.size() && i < got.size(); ++i) {
    const double reference = exact(static_cast<double>(x[i]));
    const double ulps = UlpError(got[i], reference);
    if (ulps > e.max_ulp) {
      e.max_ulp = ulps;
      e.max_ulp_at = x[i];
    }
    total += ulps;
    const double absolute = std::fabs(static_cast<double>(got[i]) - reference);
    e.max_abs = std::max(e.max_abs, absolute);
    ++e.points;
  }
  e.mean_ulp = e.points == 0 ? 0.0 : total / static_cast<double>(e.points);
  return e;
}

}  // namespace

FunctionError ExpError(std::span<const float> x, std::span<const float> got) {
  return Measure(x, got, [](double v) { return std::exp(v); });
}

FunctionError LogError(std::span<const float> x, std::span<const float> got) {
  return Measure(x, got, [](double v) { return std::log(v); });
}

std::vector<std::uint32_t> Bits(std::span<const float> values) {
  std::vector<std::uint32_t> out;
  out.reserve(values.size());
  for (const float v : values) out.push_back(FloatBits(v));
  return out;
}

std::vector<float> Floats(std::span<const std::uint32_t> bits) {
  std::vector<float> out;
  out.reserve(bits.size());
  for (const std::uint32_t b : bits) out.push_back(FromBits(b));
  return out;
}

}  // namespace aadp::metal::probes
