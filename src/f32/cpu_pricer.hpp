// The float32 line on the CPU, and what the CPU and the GPU float32 lines share on the host.
//
//   MakeParams(contract, market, seed, span)   the kernel's Params, each double rounded once, or
//   why
//                                              not: an arithmetic Asian with S0 not in the average,
//                                              or a discrete knock-out by one-step survival
//   Scale(market)                               S0 e^(-rT): the kernel prices in units of S0,
//                                              undiscounted, and the host multiplies in double
//   PriceSpansCpu(params, first_path, spans, threads)
//                                              one SpanSum per span of params.span paths, the same
//                                              for any thread count
//   Reduce(spans, span)                         the spans' sums in double, in span order
//   ToEstimate(moments, scale)                  mean and per-path sample SE, in currency
//
// The kernels are float_kernels.hpp's, run on the host's float with libm's exp, log and sqrt.
// NOTES.md, "Stage 9", is the plan.
#ifndef AADP_F32_CPU_PRICER_HPP_
#define AADP_F32_CPU_PRICER_HPP_

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "f32/float_kernels.hpp"
#include "pricer/contract.hpp"

namespace aadp::f32 {

// A float32 sum runs over one span at most: a running float32 sum stalls once its ulp passes the
// terms (metal-gpu/EVIDENCE.md, E51), so longer sums are left to the host's double.
inline constexpr std::uint32_t kMaxSpan = 64;

enum class SetupError : std::uint8_t { kNone, kContract, kProduct, kSpan };

std::expected<Params, SetupError> MakeParams(const pricer::Contract& contract,
                                             const pricer::Market<double>& market,
                                             std::uint64_t seed, std::uint32_t span);
const char* ErrorName(SetupError error);

double Scale(const pricer::Market<double>& market);

// spans * params.span paths from first_path. threads from 1 to pricer::kMaxThreads.
std::vector<SpanSum> PriceSpansCpu(const Params& params, std::uint64_t first_path,
                                   std::uint64_t spans, std::uint32_t threads);

struct Moments {
  double sum = 0.0;
  double sum_squares = 0.0;
  std::uint64_t paths = 0;
};

Moments Reduce(std::span<const SpanSum> spans, std::uint32_t span);

struct Estimate {
  double mean = 0.0;
  double standard_error = 0.0;
};

// Needs at least two paths.
Estimate ToEstimate(const Moments& moments, double scale);

}  // namespace aadp::f32

#endif  // AADP_F32_CPU_PRICER_HPP_
