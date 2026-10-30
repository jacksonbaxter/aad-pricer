#include <cstddef>
#include <cstdint>

#include "smoke/smoke.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* /*data*/, std::size_t size) {
  if (size > 0 && aadp::smoke::sum_of_ones() < 1.0) {
    __builtin_trap();
  }
  return 0;
}
