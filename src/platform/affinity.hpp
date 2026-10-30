// Thread placement. Linux can hard-pin; macOS cannot, and says so.
#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aadp::platform {

struct AffinityState {
  bool pinned = false;         // True only when a read-back mask holds exactly one CPU
  std::vector<int> cpus;       // The real mask, read back. Empty where no such mask exists
  std::optional<int> current;  // Where this thread is running now, if the OS can say
  std::string qos_class;       // macOS: an influence on placement, not a pin
};

// The state of the calling thread, whoever set it: --pin-cpu or a taskset around the binary.
AffinityState read_affinity();

// Pins the calling thread and verifies by reading the mask back. Always fails on macOS.
std::expected<AffinityState, std::string> pin_to_cpu(int cpu);

std::string pinning_state_text(const AffinityState& state);

// The nice value of this process. zsh runs background jobs at +5 by default, which is easy to
// inherit without noticing and changes what a benchmark competes with.
int process_nice();

// "4", "0-3,6". Empty input gives "none".
std::string format_cpu_list(std::span<const int> cpus);

}  // namespace aadp::platform
