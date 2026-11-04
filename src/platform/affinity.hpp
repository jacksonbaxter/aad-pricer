// Thread placement. Linux can hard-pin; macOS cannot, and says so.
#ifndef AADP_PLATFORM_AFFINITY_HPP_
#define AADP_PLATFORM_AFFINITY_HPP_

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aadp::platform {

// A threaded row on linux-zen1 runs on two named CPUs (taskset -c 4,6), so a mask of one or two
// CPUs is a pin. Anything wider is a placement left to the scheduler.
inline constexpr std::size_t kMaxPinnedCpus = 2;

// True for a read-back mask of one to kMaxPinnedCpus CPUs that leaves some online CPU out, so a
// two-CPU machine running unpinned does not read as pinned.
bool counts_as_pinned(std::span<const int> cpus, long online_cpus);

struct AffinityState {
  bool pinned = false;         // counts_as_pinned of the read-back mask
  std::vector<int> cpus;       // The real mask, read back. Empty where no such mask exists
  std::optional<int> current;  // Where this thread is running now, if the OS can say
  std::string qos_class;       // macOS: an influence on placement, not a pin
};

// The state of the calling thread, whoever set it: --pin-cpu or a taskset around the binary.
AffinityState read_affinity();

// Pins the calling thread and verifies by reading the mask back. Always fails on macOS.
std::expected<AffinityState, std::string> pin_to_cpu(int cpu);

std::string_view pinning_state_text(const AffinityState& state);

// The nice value of this process. zsh runs background jobs at +5 by default, which is easy to
// inherit without noticing and changes what a benchmark competes with.
int process_nice();

// "4", "0-3,6". Empty input gives "none".
std::string format_cpu_list(std::span<const int> cpus);

}  // namespace aadp::platform

#endif  // AADP_PLATFORM_AFFINITY_HPP_
