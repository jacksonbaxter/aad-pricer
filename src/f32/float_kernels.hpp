// float32 path kernels, one source for the CPU float32 line and the Metal GPU backend (stage 9).
//
// C++ includes this header. src/metal/ embeds its text in the library Metal compiles at run time
// (cmake/area_metal.cmake), so the CPU and the GPU run the same operations in the same order.
// Inside Metal (__METAL_VERSION__) exp, log and sqrt are metal::precise; in C++ they are the host's
// libm on float. Everything else is +, -, *, / and comparisons on float, with no contraction on
// either side (-ffp-contract=off, and `#pragma METAL fp contract(off)` in the library), so the two
// lines can differ only through those three functions, the GPU's rounding and its denormals, which
// the property kernels in tests/metal/ measure. The plan is docs/NOTES.md, "Stage 9".
//
// What both compilers accept: no pointers or references (Metal would want an address space), no
// namespace-scope variables (Metal would want them in `constant`), float literals with an F
// suffix, and value structs whose fields are all 32 bits wide, so the host's Params is the
// kernel's byte for byte.
//
// Contents:
//   PhiloxRounds, DrawBlock      Philox4x32-10 keyed as src/pricer/draws.hpp: counter (path low,
//                                path high, step, 0), key (seed low, seed high)
//   Uniform23                    (floor(w / 2^9) + 1/2) 2^-23, exact in float, never 0 or 1
//   InverseNormalF               Giles' single-precision erfinv, symmetric on the 23-bit grid
//   NormalCdfF                   Cody's erfc, as src/pricer/normal_cdf.hpp, in float
//   CompensatedAdd               Kahan's compensated sum
//   PathPayoff, PriceSpan        arithmetic Asian and one-step discrete knock-out, by span
#ifndef AADP_F32_FLOAT_KERNELS_HPP_
#define AADP_F32_FLOAT_KERNELS_HPP_

#ifndef __METAL_VERSION__
#include <cmath>
#include <cstdint>
#endif

namespace aadp::f32 {

#ifdef __METAL_VERSION__
using Word = uint;
inline float ExpF(float x) { return metal::precise::exp(x); }
inline float LogF(float x) { return metal::precise::log(x); }
inline float SqrtF(float x) { return metal::precise::sqrt(x); }
inline float TruncF(float x) { return metal::trunc(x); }
inline Word MulHi(Word a, Word b) { return metal::mulhi(a, b); }
#else
using Word = std::uint32_t;
inline float ExpF(float x) { return std::exp(x); }
inline float LogF(float x) { return std::log(x); }
inline float SqrtF(float x) { return std::sqrt(x); }
inline float TruncF(float x) { return std::trunc(x); }
inline Word MulHi(Word a, Word b) { return static_cast<Word>((std::uint64_t{a} * b) >> 32U); }
#endif

// The products this kernel prices. Params carries the value as a Word, so its layout is fixed.
// NOLINTNEXTLINE(performance-enum-size): a Params field, 32 bits on both sides of the GPU.
enum class Product : Word {
  kAsian = 0U,           // Arithmetic Asian, fixings at the m dates, S0 not in the average.
  kUpOutOneStep = 1U,    // Discrete up-and-out at the m dates, by one-step survival.
  kDownOutOneStep = 2U,  // Discrete down-and-out at the m dates, by one-step survival.
};

// One kernel's inputs, all 32-bit. The host (cpu_pricer.cpp) rounds each double once.
struct Params {
  Word product = 0U;  // Product.
  Word put = 0U;      // 0 call, 1 put.
  Word steps = 1U;    // m, below 2^24, so float(m) is exact.
  Word span = 1U;     // Paths per span, 1 to kMaxSpan.
  Word seed_lo = 0U;
  Word seed_hi = 0U;
  float drift = 0.0F;    // (r - q - sigma^2 / 2) T / m.
  float sd = 0.0F;       // sigma sqrt(T / m).
  float strike = 0.0F;   // K / S0.
  float barrier = 0.0F;  // log(H / S0), barriers only.
};

struct Words4 {
  Word w0 = 0U;
  Word w1 = 0U;
  Word w2 = 0U;
  Word w3 = 0U;
};

// Salmon et al., SC11, as src/rng/philox.hpp: the same multipliers, Weyl constants and order.
inline Words4 PhiloxRounds(int rounds, Words4 counter, Word key0, Word key1) {
  for (int round = 0; round < rounds; ++round) {
    if (round > 0) {
      key0 += 0x9E3779B9U;
      key1 += 0xBB67AE85U;
    }
    const Word high0 = MulHi(0xD2511F53U, counter.w0);
    const Word low0 = 0xD2511F53U * counter.w0;
    const Word high1 = MulHi(0xCD9E8D57U, counter.w2);
    const Word low1 = 0xCD9E8D57U * counter.w2;
    Words4 next;
    next.w0 = high1 ^ counter.w1 ^ key0;
    next.w1 = low1;
    next.w2 = high0 ^ counter.w3 ^ key1;
    next.w3 = low0;
    counter = next;
  }
  return counter;
}

// The block src/pricer/draws.hpp DrawStep takes for (seed; path, step). Its first word is the top
// 32 bits of the production pricer's 52-bit normal uniform.
inline Words4 DrawBlock(Word seed_lo, Word seed_hi, Word path_lo, Word path_hi, Word step) {
  Words4 counter;
  counter.w0 = path_lo;
  counter.w1 = path_hi;
  counter.w2 = step;
  counter.w3 = 0U;
  return PhiloxRounds(10, counter, seed_lo, seed_hi);
}

// (floor(w / 2^9) + 1/2) 2^-23, as float(2 floor(w / 2^9) + 1) 2^-24: an odd integer below 2^24
// times a power of two, so exact under either rounding mode Metal allows. From 2^-24 to
// 1 - 2^-24, never 0 or 1, and 1 - u is on the same grid. No float(uint) rounding is involved:
// float(w) / 2^32 would give 1.0 for the top 128 words under round to nearest.
inline float Uniform23(Word w) {
  return static_cast<float>(((w >> 9U) << 1U) | 1U) * 5.9604644775390625e-8F;
}

// Phi^-1(q) = sqrt(2) erfinv(2q - 1), erfinv by Giles, "Approximating the erfinv function", GPU
// Computing Gems Jade Edition (2011), Table 5, single precision, his coefficients. w is written as
// -log(4 q (1 - q)), which equals his -log((1 - x)(1 + x)) on the 23-bit grid (4q is exact, and
// 1 - q is exact there) and keeps relative accuracy for a q off it, as one-step survival draws.
// For q and 1 - q on the grid, w is the same float and x changes sign exactly, so the result is
// odd: z(1 - u) = -z(u). Fitted for w up to about 16, q from 2^-24 to 1 - 2^-24.
inline float InverseNormalF(float q) {
  const float x = 2.0F * q - 1.0F;
  float w = -LogF(4.0F * q * (1.0F - q));
  float p = 0.0F;
  if (w < 5.0F) {
    w = w - 2.5F;
    p = 2.81022636e-08F;
    p = 3.43273939e-07F + p * w;
    p = -3.5233877e-06F + p * w;
    p = -4.39150654e-06F + p * w;
    p = 0.00021858087F + p * w;
    p = -0.00125372503F + p * w;
    p = -0.00417768164F + p * w;
    p = 0.246640727F + p * w;
    p = 1.50140941F + p * w;
  } else {
    w = SqrtF(w) - 3.0F;
    p = -0.000200214257F;
    p = 0.000100950558F + p * w;
    p = 0.00134934322F + p * w;
    p = -0.00367342844F + p * w;
    p = 0.00573950773F + p * w;
    p = -0.0076224613F + p * w;
    p = 0.00943887047F + p * w;
    p = 1.00167406F + p * w;
    p = 2.83297682F + p * w;
  }
  // NOLINTNEXTLINE(modernize-use-std-numbers): Giles's constant; MSL has no std::numbers.
  return 1.41421356F * (p * x);
}

// Phi(x) = erfc(-x / sqrt 2) / 2, erfc by W. J. Cody, Math. Comp. 23 (1969) 631-637: the regions,
// coefficients, loop order and exp split of src/pricer/normal_cdf.hpp, each coefficient rounded to
// float. erfc(10.5) is about 1e-49, below half the smallest float denormal, so |y| is clamped
// there, where the double code clamps at 28. Finite x only.
inline float NormalCdfF(float x) {
  // NOLINTNEXTLINE(modernize-use-std-numbers): MSL has no std::numbers; the same float.
  const float y = -x / 1.41421356F;
  const float magnitude = y < 0.0F ? -y : y;
  const float ay = magnitude < 10.5F ? magnitude : 10.5F;

  // |y| <= 0.46875: erfc = 1 - y R(y^2), Cody's A over B.
  const float small_y = ay <= 0.46875F ? y : 0.46875F;
  const float st = small_y * small_y;
  float num = 1.85777706184603153e-1F * st;
  float den = st;
  num = (num + 3.16112374387056560e00F) * st;
  den = (den + 2.36012909523441209e01F) * st;
  num = (num + 1.13864154151050156e02F) * st;
  den = (den + 2.44024637934444173e02F) * st;
  num = (num + 3.77485237685302021e02F) * st;
  den = (den + 1.28261652607737228e03F) * st;
  const float small =
      1.0F - small_y * ((num + 3.20937758913846947e03F) / (den + 2.84423683343917062e03F));

  // Up to 4: Cody's C over D in |y|.
  const float mt = ay < 4.0F ? ay : 4.0F;
  num = 2.15311535474403846e-8F * mt;
  den = mt;
  // NOLINTNEXTLINE(modernize-use-std-numbers): Cody's fitted coefficient, not 1/sqrt(pi).
  num = (num + 5.64188496988670089e-1F) * mt;
  den = (den + 1.57449261107098347e01F) * mt;
  num = (num + 8.88314979438837594e00F) * mt;
  den = (den + 1.17693950891312499e02F) * mt;
  num = (num + 6.61191906371416295e01F) * mt;
  den = (den + 5.37181101862009858e02F) * mt;
  num = (num + 2.98635138197400131e02F) * mt;
  den = (den + 1.62138957456669019e03F) * mt;
  num = (num + 8.81952221241769090e02F) * mt;
  den = (den + 3.29079923573345963e03F) * mt;
  num = (num + 1.71204761263407058e03F) * mt;
  den = (den + 4.36261909014324716e03F) * mt;
  num = (num + 2.05107837782607147e03F) * mt;
  den = (den + 3.43936767414372164e03F) * mt;
  const float middle = (num + 1.23033935479799725e03F) / (den + 1.23033935480374942e03F);

  // Beyond 4: (1/sqrt(pi) - t R(t)) / |y| with t = 1 / y^2, Cody's P over Q.
  const float far_y = ay > 4.0F ? ay : 4.0F;
  const float ft = 1.0F / (far_y * far_y);
  num = 1.63153871373020978e-2F * ft;
  den = ft;
  num = (num + 3.05326634961232344e-1F) * ft;
  den = (den + 2.56852019228982242e00F) * ft;
  num = (num + 3.60344899949804439e-1F) * ft;
  den = (den + 1.87295284992346725e00F) * ft;
  num = (num + 1.25781726111229246e-1F) * ft;
  den = (den + 5.27905102951428412e-1F) * ft;
  num = (num + 1.60837851487422766e-2F) * ft;
  den = (den + 6.05183413124413191e-2F) * ft;
  const float far_ratio = (num + 6.58749161529837803e-4F) / (den + 2.33520497626869185e-3F);
  // NOLINTNEXTLINE(modernize-use-std-numbers): Cody's rounding of 1/sqrt(pi); MSL has none.
  const float far = (5.6418958354775628695e-1F - ft * far_ratio) / far_y;

  // exp(-y^2) as exp(-a^2) exp(-(y - a)(y + a)), a = trunc(16 y) / 16, so y^2 is never rounded.
  const float head = TruncF(ay * 16.0F) / 16.0F;
  const float rest = (ay - head) * (ay + head);
  const float ratio = ay <= 4.0F ? middle : far;
  const float upper = ExpF(-head * head) * ExpF(-rest) * ratio;
  const float tail = y < 0.0F ? 2.0F - upper : upper;
  const float complement = ay <= 0.46875F ? small : tail;
  return 0.5F * complement;
}

// Kahan's compensated sum. The library is compiled in Metal's safe math mode, which keeps the
// carry; fast math may reassociate (t - sum) - y to zero (metal-gpu/EVIDENCE.md, E6, E53).
struct Compensated {
  float sum = 0.0F;
  float carry = 0.0F;
};

inline Compensated CompensatedAdd(Compensated s, float value) {
  const float y = value - s.carry;
  const float t = s.sum + y;
  s.carry = (t - s.sum) - y;
  s.sum = t;
  return s;
}

inline float Intrinsic(Word put, float underlying, float strike) {
  const float intrinsic = put != 0U ? strike - underlying : underlying - strike;
  return intrinsic > 0.0F ? intrinsic : 0.0F;
}

// Log-moneyness x = log(S / S0) from 0, stepped as the double kernel steps, (x + mu) + s z. The
// average of S / S0 over the m dates, its sum compensated. Payoff in units of S0, undiscounted.
inline float AsianPayoff(Params p, Word path_lo, Word path_hi) {
  float x = 0.0F;
  Compensated sum;
  for (Word step = 0U; step < p.steps; ++step) {
    const float u = Uniform23(DrawBlock(p.seed_lo, p.seed_hi, path_lo, path_hi, step).w0);
    x = (x + p.drift) + p.sd * InverseNormalF(u);
    sum = CompensatedAdd(sum, ExpF(x));
  }
  return Intrinsic(p.put, sum.sum / static_cast<float>(p.steps), p.strike);
}

// One-step survival (Gerstner, Harrach, Roth, arXiv 1804.03975v3), as src/pricer/kernels.hpp
// OutOneStep: p = Phi(gap of the step's mean / sd), the weight takes p, and the step is drawn
// from its law conditioned on the live side, z = -Phi^-1(p (1 - u)) down and Phi^-1(p u) up. A
// dead step (p = 0) draws from v, as the double kernel does. The draw is floored at 2^-24, the
// bottom of the range InverseNormalF is fitted on; the double kernel has no floor, so where it
// binds it is part of this kernel's difference from double, not hidden (docs/NOTES.md, "Stage 9").
inline float OneStepPayoff(Params p, Word path_lo, Word path_hi) {
  const bool down = p.product == static_cast<Word>(Product::kDownOutOneStep);
  float x = 0.0F;
  float weight = 1.0F;
  for (Word step = 0U; step < p.steps; ++step) {
    const float u = Uniform23(DrawBlock(p.seed_lo, p.seed_hi, path_lo, path_hi, step).w0);
    const float mean = x + p.drift;
    const float gap = down ? mean - p.barrier : p.barrier - mean;
    const float survive = NormalCdfF(gap / p.sd);
    const float v = down ? 1.0F - u : u;
    const bool alive = survive > 0.0F;
    weight = weight * (alive ? survive : 0.0F);
    const float q = survive * v;
    const float floored = q > 5.9604644775390625e-8F ? q : 5.9604644775390625e-8F;
    const float conditional = InverseNormalF(alive ? floored : v);
    const float z = down ? -conditional : conditional;
    x = mean + p.sd * z;
  }
  return weight * Intrinsic(p.put, ExpF(x), p.strike);
}

inline float PathPayoff(Params p, Word path_lo, Word path_hi) {
  return p.product == static_cast<Word>(Product::kAsian) ? AsianPayoff(p, path_lo, path_hi)
                                                         : OneStepPayoff(p, path_lo, path_hi);
}

// One span: p.span consecutive paths from (first_lo, first_hi), the payoffs summed with
// compensation and their squares plainly, both in float over at most kMaxSpan terms. The host
// adds the spans in double (cpu_pricer.hpp, Reduce).
struct SpanSum {
  float sum = 0.0F;
  float sum_squares = 0.0F;
};

inline SpanSum PriceSpan(Params p, Word first_lo, Word first_hi) {
  Compensated sum;
  float squares = 0.0F;
  for (Word i = 0U; i < p.span; ++i) {
    const Word lo = first_lo + i;
    const Word hi = first_hi + (lo < first_lo ? 1U : 0U);
    const float payoff = PathPayoff(p, lo, hi);
    sum = CompensatedAdd(sum, payoff);
    squares = squares + payoff * payoff;
  }
  SpanSum out;
  out.sum = sum.sum;
  out.sum_squares = squares;
  return out;
}

}  // namespace aadp::f32

#endif  // AADP_F32_FLOAT_KERNELS_HPP_
