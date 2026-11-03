// Uniforms for one path, from Philox4x32-10 keyed on (seed; path, step), the stream of
// src/rng/philox.hpp. One block per (path, step) gives four 32-bit words: the first two make the
// step's normal uniform, bit for bit the uniform PathUniform gives the reference kernels, and the
// last two make the uniform a sampled continuous crossing needs. So the pricer and the reference
// see the same normals, and the crossing uniform is independent of them and of every other step.
#ifndef AADP_PRICER_DRAWS_HPP_
#define AADP_PRICER_DRAWS_HPP_

#include <cstdint>
#include <span>

#include "rng/philox.hpp"

namespace aadp::pricer {

struct StepUniforms {
  double normal;
  double crossing;
};

constexpr StepUniforms DrawStep(std::uint64_t seed, std::uint64_t path, std::uint32_t step) {
  const Philox4x32Output out = Philox4x32_10(
      {static_cast<std::uint32_t>(path), static_cast<std::uint32_t>(path >> 32), step, 0U},
      {static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32)});
  return {UniformFromBits((std::uint64_t{out[0]} << 32) | out[1]),
          UniformFromBits((std::uint64_t{out[2]} << 32) | out[3])};
}

// What a kernel reads: one normal uniform per step, and one crossing uniform per step for the
// contracts that sample a crossing (empty otherwise).
struct PathUniforms {
  std::span<const double> normal;
  std::span<const double> crossing;
};

// normal.size() steps. crossing is filled too unless it is empty.
inline void FillPath(std::uint64_t seed, std::uint64_t path, std::span<double> normal,
                     std::span<double> crossing) {
  for (std::size_t step = 0; step < normal.size(); ++step) {
    const StepUniforms u = DrawStep(seed, path, static_cast<std::uint32_t>(step));
    normal[step] = u.normal;
    if (!crossing.empty()) crossing[step] = u.crossing;
  }
}

}  // namespace aadp::pricer

#endif  // AADP_PRICER_DRAWS_HPP_
