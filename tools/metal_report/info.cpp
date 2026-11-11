// aadp_metal_info: prints whether this host has a Metal device, and exits 0 if it has none or if
// the backend set up on it. CI runs it before the tests, and allows the GPU tests' skip only when
// it says "metal: none" (.github/workflows/ci.yml). A device on which the backend fails to set up
// is an error, exit 1, never "none".
//
//   metal: device <name>; setup device <s> s, compile <s> s, pipelines <s> s
//   metal: none (MTLCreateSystemDefaultDevice gave nil)
//   metal: error (<reason>)
#include <cstdio>

#include "metal/metal_backend.hpp"

int main() {
  if (!aadp::metal::HasDevice()) {
    std::printf("metal: none (MTLCreateSystemDefaultDevice gave nil)\n");
    return 0;
  }
  const auto context = aadp::metal::Context::Create();
  if (!context) {
    std::printf("metal: error (%s)\n", context.error().c_str());
    return 1;
  }
  const aadp::metal::SetupTimes& setup = (*context)->Setup();
  std::printf("metal: device %s; setup device %.6f s, compile %.6f s, pipelines %.6f s\n",
              (*context)->DeviceName().c_str(), setup.device_s, setup.compile_s, setup.pipelines_s);
  return 0;
}
