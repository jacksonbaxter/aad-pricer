// The Highway target this build was compiled for. Static dispatch, so one target per binary.
#pragma once

#include <cstddef>
#include <string>

namespace aadp::platform {

std::string simd_target_name();
std::size_t simd_double_lanes();

}  // namespace aadp::platform
