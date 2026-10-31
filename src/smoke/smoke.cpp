#include "smoke/smoke.hpp"

#include <hwy/highway.h>
#include <hwy/targets.h>

#include "build_info.hpp"

// A missing ISA flag makes Highway fall back to a lesser target without complaint.
static_assert(aadp::build_info::simd_target != "scalar" || HWY_STATIC_TARGET == HWY_SCALAR);
static_assert(aadp::build_info::simd_target != "avx2" || HWY_STATIC_TARGET == HWY_AVX2);
static_assert(aadp::build_info::simd_target != "avx512" || HWY_STATIC_TARGET == HWY_AVX3);

HWY_BEFORE_NAMESPACE();

namespace aadp::smoke::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;
namespace {

double SumOfOnes() {
  const hn::ScalableTag<double> d;
  return hn::ReduceSum(d, hn::Set(d, 1.0));
}

}  // namespace

}  // namespace aadp::smoke::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

namespace aadp::smoke {

const char* highway_target_name() { return hwy::TargetName(HWY_STATIC_TARGET); }

double sum_of_ones() { return HWY_NAMESPACE::SumOfOnes(); }

}  // namespace aadp::smoke
