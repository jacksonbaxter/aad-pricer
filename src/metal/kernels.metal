// Metal entry points (stage 9). Never compiled offline: cmake/area_metal.cmake embeds this text,
// after src/f32/float_kernels.hpp, in the string src/metal/metal_backend.mm compiles at run time.
// The pricing kernel calls the shared code only; the probes measure what docs/NOTES.md, "Stage 9",
// lists. Inputs come from buffers, so the compiler cannot fold a probe.

using namespace aadp::f32;

struct DispatchArgs {
  uint first_path_lo;
  uint first_path_hi;
  uint spans;
};

// One thread per span of params.span paths, from the buffer's first path.
kernel void price_spans(constant Params& params [[buffer(0)]],
                        constant DispatchArgs& args [[buffer(1)]],
                        device SpanSum* out [[buffer(2)]],
                        uint gid [[thread_position_in_grid]]) {
  if (gid >= args.spans) return;
  const Params p = params;
  const ulong first = ((ulong(args.first_path_hi) << 32) | ulong(args.first_path_lo)) +
                      ulong(gid) * ulong(p.span);
  out[gid] = PriceSpan(p, uint(first), uint(first >> 32));
}

// (a, b) -> a + b, a - b, a * b, a / b, sqrt(|a|).
kernel void probe_arithmetic(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                             constant uint& count [[buffer(2)]],
                             uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  const float a = as_type<float>(in[2 * gid]);
  const float b = as_type<float>(in[2 * gid + 1]);
  out[5 * gid] = as_type<uint>(a + b);
  out[5 * gid + 1] = as_type<uint>(a - b);
  out[5 * gid + 2] = as_type<uint>(a * b);
  out[5 * gid + 3] = as_type<uint>(a / b);
  out[5 * gid + 4] = as_type<uint>(SqrtF(a < 0.0F ? -a : a));
}

// (a, b, c) -> a * b + c, written as the kernels write it.
kernel void probe_contraction(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                              constant uint& count [[buffer(2)]],
                              uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  const float a = as_type<float>(in[3 * gid]);
  const float b = as_type<float>(in[3 * gid + 1]);
  const float c = as_type<float>(in[3 * gid + 2]);
  out[gid] = as_type<uint>(a * b + c);
}

// (a, b) -> a * b, a + b, a > 0, a != 0: denormals as inputs and as results.
kernel void probe_pairs(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                        constant uint& count [[buffer(2)]], uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  const float a = as_type<float>(in[2 * gid]);
  const float b = as_type<float>(in[2 * gid + 1]);
  out[4 * gid] = as_type<uint>(a * b);
  out[4 * gid + 1] = as_type<uint>(a + b);
  out[4 * gid + 2] = a > 0.0F ? 1u : 0u;
  out[4 * gid + 3] = a != 0.0F ? 1u : 0u;
}

// k -> float(k), float(k) * 2^-32.
kernel void probe_uint_to_float(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                                constant uint& count [[buffer(2)]],
                                uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  const float f = float(in[gid]);
  out[2 * gid] = as_type<uint>(f);
  out[2 * gid + 1] = as_type<uint>(f * 2.3283064365386963e-10F);
}

kernel void probe_exp(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                      constant uint& count [[buffer(2)]], uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  out[gid] = as_type<uint>(ExpF(as_type<float>(in[gid])));
}

kernel void probe_exp_fast(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                           constant uint& count [[buffer(2)]],
                           uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  out[gid] = as_type<uint>(metal::fast::exp(as_type<float>(in[gid])));
}

kernel void probe_log(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                      constant uint& count [[buffer(2)]], uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  out[gid] = as_type<uint>(LogF(as_type<float>(in[gid])));
}

kernel void probe_log_fast(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                           constant uint& count [[buffer(2)]],
                           uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  out[gid] = as_type<uint>(metal::fast::log(as_type<float>(in[gid])));
}

// (start, term, count) -> the kernels' compensated sum, and the plain sum, of start followed by
// count copies of term.
kernel void probe_compensated(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                              constant uint& count [[buffer(2)]],
                              uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  const float start = as_type<float>(in[3 * gid]);
  const float term = as_type<float>(in[3 * gid + 1]);
  const uint terms = in[3 * gid + 2];
  Compensated compensated;
  compensated = CompensatedAdd(compensated, start);
  float plain = start;
  for (uint i = 0; i < terms; ++i) {
    compensated = CompensatedAdd(compensated, term);
    plain = plain + term;
  }
  out[2 * gid] = as_type<uint>(compensated.sum);
  out[2 * gid + 1] = as_type<uint>(plain);
}

// (c0, c1, c2, c3, k0, k1, rounds) -> PhiloxRounds.
kernel void probe_philox(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                         constant uint& count [[buffer(2)]], uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  Words4 counter;
  counter.w0 = in[7 * gid];
  counter.w1 = in[7 * gid + 1];
  counter.w2 = in[7 * gid + 2];
  counter.w3 = in[7 * gid + 3];
  const Words4 r = PhiloxRounds(int(in[7 * gid + 6]), counter, in[7 * gid + 4], in[7 * gid + 5]);
  out[4 * gid] = r.w0;
  out[4 * gid + 1] = r.w1;
  out[4 * gid + 2] = r.w2;
  out[4 * gid + 3] = r.w3;
}

// (seed lo, seed hi, path lo, path hi, step) -> DrawBlock, the pricing kernel's block.
kernel void probe_draw(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                       constant uint& count [[buffer(2)]], uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  const Words4 r = DrawBlock(in[5 * gid], in[5 * gid + 1], in[5 * gid + 2], in[5 * gid + 3],
                             in[5 * gid + 4]);
  out[4 * gid] = r.w0;
  out[4 * gid + 1] = r.w1;
  out[4 * gid + 2] = r.w2;
  out[4 * gid + 3] = r.w3;
}

// w -> Uniform23(w).
kernel void probe_uniform(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                          constant uint& count [[buffer(2)]], uint gid [[thread_position_in_grid]]) {
  if (gid >= count) return;
  out[gid] = as_type<uint>(Uniform23(in[gid]));
}
