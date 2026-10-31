#ifndef AADP_PLATFORM_MEMORY_INFO_HPP_
#define AADP_PLATFORM_MEMORY_INFO_HPP_

#include <cstdint>

namespace aadp::platform {

// Peak resident set of this process in bytes. getrusage reports bytes on macOS and KiB on
// Linux; this hides that.
std::uint64_t peak_rss_bytes();

}  // namespace aadp::platform

#endif  // AADP_PLATFORM_MEMORY_INFO_HPP_
