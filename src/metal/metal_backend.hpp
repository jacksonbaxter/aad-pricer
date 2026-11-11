// The Metal GPU backend: float32 prices only, behind a plain C++ interface (stage 9).
//
// AAD does not run on the GPU. Greeks come from the double-precision CPU tape. This backend runs
// src/f32/float_kernels.hpp, embedded in a library compiled at run time (this host has no offline
// `metal` compiler), with MTLMathModeSafe, MTLMathFloatingPointFunctionsPrecise and contraction
// off. Built only with AAD_PRICER_ENABLE_METAL (cmake/area_metal.cmake); the Objective-C++ is in
// metal_backend.mm, under ARC.
//
//   Context::Create()          the system default device, the library and every pipeline, or why
//                              not. "no Metal device" when MTLCreateSystemDefaultDevice gives nil,
//                              as a build can have Metal and its host no GPU (a CI virtual machine)
//   Price(params, paths, paths_per_buffer)
//                              span sums of paths [0, paths), in command buffers of
//                              paths_per_buffer, every buffer's status and error checked, reduced
//                              on the host in double. wall_s runs from before the output buffer is
//                              allocated to after the reduction
//   Probe(kernel, in, out_words, threads)
//                              one of the property kernels, on 32-bit words
#ifndef AADP_METAL_METAL_BACKEND_HPP_
#define AADP_METAL_METAL_BACKEND_HPP_

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "f32/cpu_pricer.hpp"
#include "f32/float_kernels.hpp"

namespace aadp::metal {

// Set up once, outside any timed run (BENCHMARKS.md GPU-1 reports them apart).
struct SetupTimes {
  double device_s = 0.0;     // MTLCreateSystemDefaultDevice and the command queue.
  double compile_s = 0.0;    // newLibraryWithSource from the embedded string.
  double pipelines_s = 0.0;  // Every function and compute pipeline.
};

struct BufferTime {
  std::uint64_t paths = 0;
  double gpu_s = 0.0;  // GPUEndTime - GPUStartTime: a breakdown, never the headline.
};

struct PriceRun {
  f32::Moments moments;             // Reduced on the host in double, in span order.
  double wall_s = 0.0;              // Allocation to reduction, all buffers.
  std::vector<BufferTime> buffers;  // In commit order.
  std::vector<f32::SpanSum> spans;  // Copied after the clock stops, when asked for.
};

// The property kernels of NOTES.md, "Stage 9". Each reads and writes 32-bit words; see
// src/metal/kernels.metal for each one's layout.
enum class ProbeKernel : std::uint8_t {
  kArithmetic,   // (a, b) -> a + b, a - b, a * b, a / b, sqrt(|a|)
  kContraction,  // (a, b, c) -> a * b + c
  kPairs,        // (a, b) -> a * b, a + b, a > 0, a != 0
  kUintToFloat,  // k -> float(k), float(k) * 2^-32
  kExp,          // x -> the kernels' exp
  kExpFast,      // x -> metal::fast::exp
  kLog,          // x -> the kernels' log
  kLogFast,      // x -> metal::fast::log
  kCompensated,  // (start, term, count) -> compensated sum, plain sum; one thread
  kPhilox,       // (c0, c1, c2, c3, k0, k1, rounds) -> four words
  kDraw,         // (seed lo, seed hi, path lo, path hi, step) -> DrawBlock's four words
  kUniform,      // w -> Uniform23(w)
};

class Context {
 public:
  static std::expected<std::unique_ptr<Context>, std::string> Create();
  ~Context();
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  Context(Context&&) = delete;
  Context& operator=(Context&&) = delete;

  [[nodiscard]] std::string DeviceName() const;
  [[nodiscard]] const SetupTimes& Setup() const;

  // paths and paths_per_buffer must be positive multiples of params.span.
  std::expected<PriceRun, std::string> Price(const f32::Params& params, std::uint64_t paths,
                                             std::uint64_t paths_per_buffer, bool keep_spans);

  // threads items; in must hold threads times the kernel's input words, and out_words is
  // threads times its output words.
  std::expected<std::vector<std::uint32_t>, std::string> Probe(ProbeKernel kernel,
                                                               std::span<const std::uint32_t> in,
                                                               std::size_t out_words,
                                                               std::uint32_t threads);

 private:
  struct Impl;
  explicit Context(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// The library source the backend compiles, for the record and for tests.
const char* LibrarySource();

// Whether MTLCreateSystemDefaultDevice gives a device. Only its absence may excuse a GPU test:
// with a device, a Create that fails is a failure.
bool HasDevice();

}  // namespace aadp::metal

#endif  // AADP_METAL_METAL_BACKEND_HPP_
