// Inputs and CPU references for the property kernels of NOTES.md, "Stage 9", shared by the checks
// (tests/metal/) and the report (tools/metal_report/). Pure C++: the GPU side is Context::Probe.
//
// Every grid and operand set here is the pre-registered one. Random operands come from Philox,
// keyed on the property seed, so a run is reproducible.
#ifndef AADP_METAL_PROBES_HPP_
#define AADP_METAL_PROBES_HPP_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aadp::metal::probes {

// The k-th 32-bit word of the property stream: Philox4x32-10 of (k, 0, tag, 0) under the seed.
std::uint32_t RandomWord(std::uint64_t seed, std::uint64_t k, std::uint32_t tag);

// Operand pairs, as float bit patterns: 2^20 random ones (random sign and mantissa, exponent from
// -30 to 30, so every result of + - * / and sqrt is normal), then the constructed ties and
// near-ties around 1 and 2^24, named by ConstructedNames.
std::vector<std::uint32_t> ArithmeticInputs(std::uint64_t seed, std::uint32_t random_pairs);
std::vector<std::string> ConstructedNames();
// The CPU's IEEE round-to-nearest-even results, five words per pair: a + b, a - b, a * b, a / b,
// sqrt(|a|).
std::vector<std::uint32_t> ArithmeticReference(std::span<const std::uint32_t> pairs);

// Triples (a, b, c) and the unfused a * b + c, two roundings.
std::vector<std::uint32_t> ContractionInputs(std::uint64_t seed, std::uint32_t triples);
std::vector<std::uint32_t> ContractionReference(std::span<const std::uint32_t> triples);

// Words for the uint-to-float probe: 2^24 + 1, 2^24 + 3, 2^32 - 129, 2^32 - 128, 2^32 - 1, then
// random ones.
std::vector<std::uint32_t> UintInputs(std::uint64_t seed, std::uint32_t random_words);

// Pairs for the denormal probe: (x, 1) and (x, 0) for four denormal x, then (2^-126, 2^-k) for
// k = 1, 10 and 23, whose product is denormal. DenormalNames names them.
std::vector<std::uint32_t> DenormalInputs();
std::vector<std::string> DenormalNames();

// The exp and log grids, as floats (each double grid point rounded to nearest float).
enum class Grid : std::uint8_t {
  kExpWide,    // float(-87 + 175 i / 2^22), i = 0 .. 2^22
  kExpPricer,  // float(-3 + 6 i / 2^22)
  kLogWide,    // float(2^t), t = -126 + 254 i / 2^22
  kLogUnit,    // float(0.5 + 1.5 i / 2^22)
};
std::vector<float> GridPoints(Grid grid);
const char* GridName(Grid grid);

// The error of a float result against an exact value, in float ULPs at the exact value: the
// spacing of floats at |exact|, the smallest normal's below it. Infinite for a non-finite got.
double UlpError(float got, double exact);

struct FunctionError {
  double max_ulp = 0.0;
  double max_ulp_at = 0.0;
  double mean_ulp = 0.0;
  double max_abs = 0.0;  // |got - exact|, for log on [0.5, 2].
  std::uint64_t points = 0;
};

// got[i] against exact(x[i]) for exp or log (double, the host's libm).
FunctionError ExpError(std::span<const float> x, std::span<const float> got);
FunctionError LogError(std::span<const float> x, std::span<const float> got);

std::vector<std::uint32_t> Bits(std::span<const float> values);
std::vector<float> Floats(std::span<const std::uint32_t> bits);

}  // namespace aadp::metal::probes

#endif  // AADP_METAL_PROBES_HPP_
