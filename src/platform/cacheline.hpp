// Project cache-line constant, used for layout. std::hardware_destructive_interference_size
// is deliberately not used: it reports 256 with Apple clang here against a real line of 128,
// and its value moves with -mtune, which would change ABI between builds.
#ifndef AADP_PLATFORM_CACHELINE_HPP_
#define AADP_PLATFORM_CACHELINE_HPP_

#include <cstddef>

namespace aadp::platform {

#if defined(__aarch64__) || defined(__arm64__)
inline constexpr std::size_t kCacheLineBytes = 128;
#elifdef __x86_64__
inline constexpr std::size_t kCacheLineBytes = 64;
#else
#error "Only arm64 and x86-64 are supported"
#endif

}  // namespace aadp::platform

#endif  // AADP_PLATFORM_CACHELINE_HPP_
