// The counters of the replaced global allocation functions in allocation_test.cpp, for the other
// sources of the same binary (tests/simd/lane_allocation_test.cpp). Counting is on while
// `counting` is true; `allocations` is the number of allocations counted.
#ifndef AADP_TESTS_AD_ALLOCATION_COUNTER_HPP_
#define AADP_TESTS_AD_ALLOCATION_COUNTER_HPP_

#include <atomic>
#include <cstdint>

namespace aadp::testing::allocation {

// The replaced operator new has no way to reach state but a global.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables): see above.
extern std::atomic<bool> counting;
extern std::atomic<std::uint64_t> allocations;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace aadp::testing::allocation

#endif  // AADP_TESTS_AD_ALLOCATION_COUNTER_HPP_
