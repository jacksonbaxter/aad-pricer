#include <cstdio>
#include <string_view>

#include "build_info.hpp"
#include "smoke/smoke.hpp"

int main() {
  namespace info = aadp::build_info;
  const auto print = [](const char* key, std::string_view value) {
    std::printf("%s: %.*s\n", key, static_cast<int>(value.size()), value.data());
  };
  print("compiler", info::compiler_id);
  print("version", info::compiler_version);
  print("flags", info::cxx_flags);
  print("build type", info::build_type);
  print("simd target", info::simd_target);
  print("highway target", aadp::smoke::highway_target_name());
  std::printf("c++ standard: %d\n", info::cxx_standard);
}
