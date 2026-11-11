// Objective-C++ under ARC behind metal_backend.hpp. Nothing here does arithmetic on prices: the
// kernels are src/f32/float_kernels.hpp, and the reduction is f32::Reduce, in double.
#include "metal/metal_backend.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "f32/cpu_pricer.hpp"
#include "f32/float_kernels.hpp"

namespace aadp::metal {

namespace {

using Clock = std::chrono::steady_clock;

double Seconds(Clock::time_point from, Clock::time_point to) {
  return std::chrono::duration<double>(to - from).count();
}

// src/metal/kernels.metal's DispatchArgs.
struct DispatchArgs {
  std::uint32_t first_path_lo = 0;
  std::uint32_t first_path_hi = 0;
  std::uint32_t spans = 0;
};

// The kernel reads Params from a constant buffer with Metal's layout: ten 32-bit fields.
static_assert(sizeof(f32::Params) == 40 && alignof(f32::Params) == 4);
static_assert(sizeof(f32::SpanSum) == 8 && sizeof(DispatchArgs) == 12);

struct ProbeShape {
  const char* name;
  std::size_t in_words;
  std::size_t out_words;
};

ProbeShape ShapeOf(ProbeKernel kernel) {
  switch (kernel) {
    case ProbeKernel::kArithmetic:
      return {.name = "probe_arithmetic", .in_words = 2, .out_words = 5};
    case ProbeKernel::kContraction:
      return {.name = "probe_contraction", .in_words = 3, .out_words = 1};
    case ProbeKernel::kPairs:
      return {.name = "probe_pairs", .in_words = 2, .out_words = 4};
    case ProbeKernel::kUintToFloat:
      return {.name = "probe_uint_to_float", .in_words = 1, .out_words = 2};
    case ProbeKernel::kExp:
      return {.name = "probe_exp", .in_words = 1, .out_words = 1};
    case ProbeKernel::kExpFast:
      return {.name = "probe_exp_fast", .in_words = 1, .out_words = 1};
    case ProbeKernel::kLog:
      return {.name = "probe_log", .in_words = 1, .out_words = 1};
    case ProbeKernel::kLogFast:
      return {.name = "probe_log_fast", .in_words = 1, .out_words = 1};
    case ProbeKernel::kCompensated:
      return {.name = "probe_compensated", .in_words = 3, .out_words = 2};
    case ProbeKernel::kPhilox:
      return {.name = "probe_philox", .in_words = 7, .out_words = 4};
    case ProbeKernel::kDraw:
      return {.name = "probe_draw", .in_words = 5, .out_words = 4};
    case ProbeKernel::kUniform:
      return {.name = "probe_uniform", .in_words = 1, .out_words = 1};
  }
  return {.name = "", .in_words = 0, .out_words = 0};
}

constexpr const char* kPriceKernel = "price_spans";
constexpr std::array<ProbeKernel, 12> kAllProbes = {
    ProbeKernel::kArithmetic,  ProbeKernel::kContraction, ProbeKernel::kPairs,
    ProbeKernel::kUintToFloat, ProbeKernel::kExp,         ProbeKernel::kExpFast,
    ProbeKernel::kLog,         ProbeKernel::kLogFast,     ProbeKernel::kCompensated,
    ProbeKernel::kPhilox,      ProbeKernel::kDraw,        ProbeKernel::kUniform};

std::string Describe(NSError* error) {
  if (error == nil) return "no error object";
  return std::string(error.localizedDescription.UTF8String) + " (domain " +
         error.domain.UTF8String + ", code " + std::to_string(error.code) + ")";
}

std::string_view StatusName(MTLCommandBufferStatus status) {
  switch (status) {
    case MTLCommandBufferStatusNotEnqueued:
      return "not enqueued";
    case MTLCommandBufferStatusEnqueued:
      return "enqueued";
    case MTLCommandBufferStatusCommitted:
      return "committed";
    case MTLCommandBufferStatusScheduled:
      return "scheduled";
    case MTLCommandBufferStatusCompleted:
      return "completed";
    case MTLCommandBufferStatusError:
      return "error";
  }
  return "unknown";
}

// Every buffer is checked, not just the last: a buffer the system aborts (a timeout, E66) reports
// it only in its own status and error.
std::expected<double, std::string> Finished(id<MTLCommandBuffer> buffer, std::size_t index) {
  [buffer waitUntilCompleted];
  if (buffer.status != MTLCommandBufferStatusCompleted || buffer.error != nil) {
    return std::unexpected("command buffer " + std::to_string(index) + " ended " +
                           std::string(StatusName(buffer.status)) + ": " + Describe(buffer.error));
  }
  return buffer.GPUEndTime - buffer.GPUStartTime;
}

}  // namespace

struct Context::Impl {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLLibrary> library = nil;
  id<MTLComputePipelineState> price = nil;
  NSMutableDictionary<NSString*, id<MTLComputePipelineState>>* probes = nil;
  SetupTimes setup;
};

bool HasDevice() {
  @autoreleasepool {
    return MTLCreateSystemDefaultDevice() != nil;
  }
}

Context::Context(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Context::~Context() = default;

std::expected<std::unique_ptr<Context>, std::string> Context::Create() {
  @autoreleasepool {
    auto impl = std::make_unique<Impl>();
    const Clock::time_point start = Clock::now();
    impl->device = MTLCreateSystemDefaultDevice();
    if (impl->device == nil) {
      return std::unexpected(std::string("no Metal device: MTLCreateSystemDefaultDevice gave nil"));
    }
    impl->queue = [impl->device newCommandQueue];
    if (impl->queue == nil) return std::unexpected(std::string("no command queue"));
    const Clock::time_point device_done = Clock::now();

    MTLCompileOptions* const options = [[MTLCompileOptions alloc] init];
    options.mathMode = MTLMathModeSafe;
    options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
    NSString* const source = [NSString stringWithUTF8String:LibrarySource()];
    if (source == nil) return std::unexpected(std::string("the library source is not UTF-8"));
    NSError* error = nil;
    impl->library = [impl->device newLibraryWithSource:source options:options error:&error];
    if (impl->library == nil) {
      return std::unexpected("the library did not compile: " + Describe(error));
    }
    const Clock::time_point compiled = Clock::now();

    const auto pipeline =
        [&](const char* name) -> std::expected<id<MTLComputePipelineState>, std::string> {
      NSString* const key = [NSString stringWithUTF8String:name];
      if (key == nil) return std::unexpected(std::string("a kernel name is not UTF-8"));
      id<MTLFunction> const function = [impl->library newFunctionWithName:key];
      if (function == nil) return std::unexpected(std::string("no function ") + name);
      NSError* pipeline_error = nil;
      id<MTLComputePipelineState> const state =
          [impl->device newComputePipelineStateWithFunction:function error:&pipeline_error];
      if (state == nil) {
        return std::unexpected(std::string("no pipeline for ") + name + ": " +
                               Describe(pipeline_error));
      }
      return state;
    };
    auto price = pipeline(kPriceKernel);
    if (!price) return std::unexpected(price.error());
    impl->price = *price;
    impl->probes = [NSMutableDictionary dictionary];
    for (const ProbeKernel kernel : kAllProbes) {
      const char* name = ShapeOf(kernel).name;
      auto state = pipeline(name);
      if (!state) return std::unexpected(state.error());
      NSString* const key = [NSString stringWithUTF8String:name];
      if (key == nil) return std::unexpected(std::string("a kernel name is not UTF-8"));
      impl->probes[key] = *state;
    }
    const Clock::time_point pipelines_done = Clock::now();
    impl->setup = {.device_s = Seconds(start, device_done),
                   .compile_s = Seconds(device_done, compiled),
                   .pipelines_s = Seconds(compiled, pipelines_done)};
    return std::unique_ptr<Context>(new Context(std::move(impl)));
  }
}

std::string Context::DeviceName() const { return impl_->device.name.UTF8String; }

const SetupTimes& Context::Setup() const { return impl_->setup; }

std::expected<PriceRun, std::string> Context::Price(const f32::Params& params, std::uint64_t paths,
                                                    std::uint64_t paths_per_buffer,
                                                    bool keep_spans) {
  const std::uint64_t span = params.span;
  if (span < 1 || span > f32::kMaxSpan) return std::unexpected(std::string("span outside 1 to 64"));
  if (paths == 0 || paths % span != 0 || paths_per_buffer == 0 || paths_per_buffer % span != 0) {
    return std::unexpected(std::string("paths and paths per buffer must be multiples of the span"));
  }
  const std::uint64_t spans = paths / span;
  const std::uint64_t spans_per_buffer = std::min(paths_per_buffer / span, spans);
  if (spans_per_buffer > UINT32_MAX) {
    return std::unexpected(std::string("more than 2^32 - 1 spans in one command buffer"));
  }
  @autoreleasepool {
    PriceRun run;
    const Clock::time_point start = Clock::now();
    id<MTLBuffer> const out = [impl_->device newBufferWithLength:spans * sizeof(f32::SpanSum)
                                                         options:MTLResourceStorageModeShared];
    if (out == nil) return std::unexpected(std::string("could not allocate the output buffer"));
    NSMutableArray<id<MTLCommandBuffer>>* const committed = [NSMutableArray array];
    const NSUInteger group =
        std::min<NSUInteger>(impl_->price.maxTotalThreadsPerThreadgroup, NSUInteger{256});
    for (std::uint64_t first = 0; first < spans; first += spans_per_buffer) {
      const std::uint64_t count = std::min(spans_per_buffer, spans - first);
      const std::uint64_t first_path = first * span;
      const DispatchArgs args{.first_path_lo = static_cast<std::uint32_t>(first_path),
                              .first_path_hi = static_cast<std::uint32_t>(first_path >> 32U),
                              .spans = static_cast<std::uint32_t>(count)};
      id<MTLCommandBuffer> const buffer = [impl_->queue commandBuffer];
      id<MTLComputeCommandEncoder> const encoder = [buffer computeCommandEncoder];
      [encoder setComputePipelineState:impl_->price];
      [encoder setBytes:&params length:sizeof(params) atIndex:0];
      [encoder setBytes:&args length:sizeof(args) atIndex:1];
      [encoder setBuffer:out offset:first * sizeof(f32::SpanSum) atIndex:2];
      [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
      [encoder endEncoding];
      [buffer commit];
      [committed addObject:buffer];
      run.buffers.push_back({.paths = count * span, .gpu_s = 0.0});
    }
    for (std::size_t i = 0; i < run.buffers.size(); ++i) {
      const auto gpu = Finished(committed[i], i);
      if (!gpu) return std::unexpected(gpu.error());
      run.buffers[i].gpu_s = *gpu;
    }
    const std::span<const f32::SpanSum> result(static_cast<const f32::SpanSum*>(out.contents),
                                               static_cast<std::size_t>(spans));
    run.moments = f32::Reduce(result, params.span);
    run.wall_s = Seconds(start, Clock::now());
    if (keep_spans) run.spans.assign(result.begin(), result.end());
    return run;
  }
}

std::expected<std::vector<std::uint32_t>, std::string> Context::Probe(
    ProbeKernel kernel, std::span<const std::uint32_t> in, std::size_t out_words,
    std::uint32_t threads) {
  const ProbeShape shape = ShapeOf(kernel);
  if (threads == 0 || in.size() != threads * shape.in_words ||
      out_words != threads * shape.out_words) {
    return std::unexpected(std::string("wrong word counts for ") + shape.name);
  }
  @autoreleasepool {
    NSString* const key = [NSString stringWithUTF8String:shape.name];
    id<MTLComputePipelineState> const state = key == nil ? nil : impl_->probes[key];
    if (state == nil) return std::unexpected(std::string("no pipeline for ") + shape.name);
    id<MTLBuffer> const input = [impl_->device newBufferWithBytes:in.data()
                                                           length:in.size_bytes()
                                                          options:MTLResourceStorageModeShared];
    id<MTLBuffer> const output =
        [impl_->device newBufferWithLength:out_words * sizeof(std::uint32_t)
                                   options:MTLResourceStorageModeShared];
    if (input == nil || output == nil) {
      return std::unexpected(std::string("could not allocate the probe buffers"));
    }
    id<MTLCommandBuffer> const buffer = [impl_->queue commandBuffer];
    id<MTLComputeCommandEncoder> const encoder = [buffer computeCommandEncoder];
    [encoder setComputePipelineState:state];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder setBytes:&threads length:sizeof(threads) atIndex:2];
    const NSUInteger group = std::min<NSUInteger>(state.maxTotalThreadsPerThreadgroup, 256);
    [encoder dispatchThreads:MTLSizeMake(threads, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
    [encoder endEncoding];
    [buffer commit];
    const auto done = Finished(buffer, 0);
    if (!done) return std::unexpected(done.error());
    const auto* words = static_cast<const std::uint32_t*>(output.contents);
    return std::vector<std::uint32_t>(words, words + out_words);
  }
}

}  // namespace aadp::metal
