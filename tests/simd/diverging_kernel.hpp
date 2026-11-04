// A kernel no pricer product has, for tests and the fallback timing only: at the end, a Select
// between two different active statements, on the first uniform. Lanes whose first uniforms fall
// on both sides of the threshold diverge, so their group falls back to the scalar tape. At a
// threshold of 1 or more every lane takes the first statement, the same work with no divergence.
#ifndef AADP_TESTS_SIMD_DIVERGING_KERNEL_HPP_
#define AADP_TESTS_SIMD_DIVERGING_KERNEL_HPP_

#include <hwy/highway.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "ad/gradient.hpp"
#include "ad/real.hpp"
#include "ad/tape.hpp"
#include "pricer/contract.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/runner.hpp"
#include "simd/lane_kernels.hpp"
#include "simd/lane_real.hpp"
#include "simd/lane_tape_ops.hpp"

HWY_BEFORE_NAMESPACE();

namespace aadp::simd::testing {

struct DivergingKernel {
  double threshold = 0.5;

  template <class Real, class Model>
  [[nodiscard]] Real Lanes(const pricer::Contract& c, const Model& model,
                           const LaneUniforms<Real>& u) const {
    Real x = model.LogSpot();
    for (std::uint32_t i = 0; i < c.steps; ++i) {
      const pricer::StepLaw<Real> law = model.Law(i, x);
      x = law.mean + law.sd * InverseNormal(u.normal[i]);
    }
    const Real a = Exp(x);
    const Real b = a * 1.5;
    return model.Discount() * Select(u.normal[0] < threshold, a, b);
  }
  [[nodiscard]] ad::Real Scalar(const pricer::Contract& c, const pricer::GbmModel<ad::Real>& model,
                                const pricer::PathUniforms& u) const {
    ad::Real x = model.LogSpot();
    for (std::uint32_t i = 0; i < c.steps; ++i) {
      const pricer::StepLaw<ad::Real> law = model.Law(i, x);
      x = law.mean + law.sd * InverseNormal(ad::Real(u.normal[i]));
    }
    const ad::Real a = Exp(x);
    const ad::Real b = a * 1.5;
    return model.Discount() * Select(u.normal[0] < threshold, a, b);
  }
};

// RunBatches on the scalar tape with the same kernel, as ad::GradientPathFn runs it.
class ScalarKernelFn {
 public:
  ScalarKernelFn(const pricer::Contract& c, const pricer::Market<double>& m,
                 const DivergingKernel& kernel = {})
      : contract_(c), kernel_(kernel), path_(c, m) {}
  void operator()(const pricer::PathUniforms& u, std::array<double, ad::kGradientStats>& stats) {
    stats = {};
    ad::Tape& tape = path_.tape();
    const ad::Real payoff = kernel_.Scalar(contract_, path_.model(), u);
    tape.AddAdjoint(payoff, 1.0);
    tape.SweepBack(tape.GetPosition(), path_.mark());
    path_.Rewind();
    stats[0] = payoff.value();
  }
  void EndBlock(std::array<double, ad::kGradientStats>& sum) {
    const std::array<double, ad::kInputs> gradient = path_.SweepSetup();
    for (std::size_t i = 0; i < ad::kInputs; ++i) {
      sum[1 + i] += i == ad::kMaturityInput ? -gradient[i] : gradient[i];
    }
  }

 private:
  pricer::Contract contract_;
  DivergingKernel kernel_;
  ad::PathGradient path_;
};

}  // namespace aadp::simd::testing

HWY_AFTER_NAMESPACE();

#endif  // AADP_TESTS_SIMD_DIVERGING_KERNEL_HPP_
