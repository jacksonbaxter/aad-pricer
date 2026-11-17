// xoshiro256++, scalar: an evaluated CPU-only option beside Philox4x32-10 (BENCHMARKS.md RNG-2).
// The pricer does not use it, and no pricing draw comes from it: it has a sequential state, so a
// draw depends on every draw before it, where Philox's depends only on its key and counter.
//
// Written from the generator's description in D. Blackman and S. Vigna, "Scrambled Linear
// Pseudorandom Number Generators", arXiv 1805.01407: the xoshiro256 linear engine with shift
// a = 17 and rotation b = 45 on four 64-bit words, and the ++ scrambler, rotl(s0 + s3, 23) + s0,
// taken from the state before the step. The state must not be all zero.
//
// Known-answer values: tests/oracle/xoshiro256pp_test.cpp.
#ifndef AADP_RNG_XOSHIRO256PP_HPP_
#define AADP_RNG_XOSHIRO256PP_HPP_

#include <array>
#include <bit>
#include <cstdint>

namespace aadp {

class Xoshiro256PlusPlus {
 public:
  using State = std::array<std::uint64_t, 4>;

  explicit constexpr Xoshiro256PlusPlus(const State& state) : s_(state) {}

  // The next 64 bits, and one step of the engine.
  constexpr std::uint64_t Next() {
    const std::uint64_t out = std::rotl(s_[0] + s_[3], 23) + s_[0];
    const std::uint64_t shifted = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= shifted;
    s_[3] = std::rotl(s_[3], 45);
    return out;
  }

  [[nodiscard]] constexpr const State& state() const { return s_; }

 private:
  State s_;
};

}  // namespace aadp

#endif  // AADP_RNG_XOSHIRO256PP_HPP_
