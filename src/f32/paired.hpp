// The double columns of the stage 9 table, and the paired comparison of any two columns.
//
//   UniformFromWord(w, bits)   (w + 1/2) 2^-32, or (floor(w / 2^9) + 1/2) 2^-23: Philox's first
//                              word, the top 32 bits of the production 52-bit uniform, truncated
//   PriceSpansDouble(...)      the production double kernels (pricer::EvaluatePath<double> on
//                              GbmModel<double>) on those uniforms, discounted, summed per span in
//                              double: columns A (32-bit) and B (23-bit)
//   FromFloat(spans, scale)    a float32 line's spans in currency, for comparison
//   ColumnEstimate(column)     mean and per-path sample SE
//   PairedDifference(a, b)     mean of a - b per path, with its SE from the per-span differences
//                              of the same paths: sd(d_span) / (S sqrt(N / S))
//
// docs/NOTES.md, "Stage 9", pre-registers the columns, the uniforms and the SEs.
#ifndef AADP_F32_PAIRED_HPP_
#define AADP_F32_PAIRED_HPP_

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "f32/cpu_pricer.hpp"
#include "f32/float_kernels.hpp"
#include "pricer/contract.hpp"

namespace aadp::f32 {

enum class UniformBits : std::uint8_t { k32, k23 };

double UniformFromWord(std::uint32_t word, UniformBits bits);

// Per-span sums of one column, in currency. span paths each.
struct Column {
  std::vector<double> sum;
  std::vector<double> sum_squares;
  std::uint32_t span = 1;
};

// spans * span paths from path 0. Needs the contract to validate, and span from 1 to kMaxSpan.
std::expected<Column, SetupError> PriceSpansDouble(const pricer::Contract& contract,
                                                   const pricer::Market<double>& market,
                                                   std::uint64_t seed, std::uint64_t spans,
                                                   std::uint32_t span, UniformBits bits,
                                                   std::uint32_t threads);

Column FromFloat(std::span<const SpanSum> spans, std::uint32_t span, double scale);

Estimate ColumnEstimate(const Column& column);

struct Difference {
  double mean = 0.0;
  double standard_error = 0.0;
};

// a - b. Both columns must have the same span and number of spans, at least two.
Difference PairedDifference(const Column& a, const Column& b);

}  // namespace aadp::f32

#endif  // AADP_F32_PAIRED_HPP_
