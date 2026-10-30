#include "platform/simd_info.hpp"

#include <hwy/highway.h>
#include <hwy/targets.h>

HWY_BEFORE_NAMESPACE();
namespace aadp::platform {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

std::size_t DoubleLanes() { return hn::Lanes(hn::ScalableTag<double>()); }

}  // namespace HWY_NAMESPACE
}  // namespace aadp::platform
HWY_AFTER_NAMESPACE();

namespace aadp::platform {

std::string simd_target_name() { return hwy::TargetName(HWY_STATIC_TARGET); }

std::size_t simd_double_lanes() { return HWY_NAMESPACE::DoubleLanes(); }

}  // namespace aadp::platform
