#include "platform/host_label.hpp"

#include "platform/cpu_info.hpp"

namespace aadp::platform {

std::string_view classify_host(std::string_view os_name, std::string_view cpu_model) {
  if (os_name == "macOS" && cpu_model == "Apple M4 Max") {
    return kHostMacM4Max;
  }
  // /proc/cpuinfo appends " with Radeon Vega Gfx" on this part.
  constexpr std::string_view kZen1 = "AMD Ryzen Embedded V1605B";
  if (os_name == "Linux" && cpu_model.starts_with(kZen1) &&
      (cpu_model.size() == kZen1.size() || cpu_model[kZen1.size()] == ' ')) {
    return kHostLinuxZen1;
  }
  return kHostOther;
}

std::string detect_host_label() {
  const CpuInfo info = read_cpu_info();
  return std::string(classify_host(info.os_name, info.cpu_model));
}

}  // namespace aadp::platform
