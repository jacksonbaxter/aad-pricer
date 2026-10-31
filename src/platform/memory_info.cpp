#include "platform/memory_info.hpp"

#include <sys/resource.h>

#include <cstdint>

namespace aadp::platform {

std::uint64_t peak_rss_bytes() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): glibc declares it inside a union.
  const long peak = usage.ru_maxrss;
  if (peak < 0) {
    return 0;
  }
  const auto value = static_cast<std::uint64_t>(peak);
#ifdef __APPLE__
  return value;
#else
  return value * 1024;
#endif
}

}  // namespace aadp::platform
