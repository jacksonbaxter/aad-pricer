#include "platform/affinity.hpp"

#include <sys/resource.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <system_error>

#ifdef __linux__
#include <sched.h>
#elifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace aadp::platform {

AffinityState read_affinity() {
  AffinityState state;
#ifdef __linux__
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (sched_getaffinity(0, sizeof(mask), &mask) == 0) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (CPU_ISSET(cpu, &mask)) {
        state.cpus.push_back(cpu);
      }
    }
  }
  state.pinned = state.cpus.size() == 1;
  const int current = sched_getcpu();
  if (current >= 0) {
    state.current = current;
  }
  state.qos_class = "not applicable";
#elifdef __APPLE__
  qos_class_t qos = QOS_CLASS_UNSPECIFIED;
  int relative = 0;
  pthread_get_qos_class_np(pthread_self(), &qos, &relative);
  state.qos_class = "0x" + [&] {
    std::array<char, 16> text{};
    std::snprintf(text.data(), text.size(), "%x", static_cast<unsigned>(qos));
    return std::string(text.data());
  }() + " (influence on placement, not a pin)";
#endif
  return state;
}

std::expected<AffinityState, std::string> pin_to_cpu(int cpu) {
#ifdef __linux__
  if (cpu < 0 || cpu >= CPU_SETSIZE) {
    return std::unexpected("CPU " + std::to_string(cpu) + " is out of range");
  }
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
    return std::unexpected("sched_setaffinity(" + std::to_string(cpu) +
                           "): " + std::generic_category().message(errno));
  }
  AffinityState state = read_affinity();
  if (state.cpus != std::vector<int>{cpu}) {
    return std::unexpected("read-back mask is " + format_cpu_list(state.cpus) + ", wanted " +
                           std::to_string(cpu));
  }
  return state;
#else
  return std::unexpected("macOS has no way to pin a thread to CPU " + std::to_string(cpu));
#endif
}

int process_nice() {
  errno = 0;
  return getpriority(PRIO_PROCESS, 0);
}

std::string pinning_state_text(const AffinityState& state) {
  return state.pinned ? "pinned" : "not pinned";
}

std::string format_cpu_list(std::span<const int> cpus) {
  if (cpus.empty()) {
    return "none";
  }
  std::string out;
  for (std::size_t i = 0; i < cpus.size();) {
    std::size_t last = i;
    while (last + 1 < cpus.size() && cpus[last + 1] == cpus[last] + 1) {
      ++last;
    }
    if (!out.empty()) {
      out += ',';
    }
    out += std::to_string(cpus[i]);
    if (last != i) {
      out += '-';
      out += std::to_string(cpus[last]);
    }
    i = last + 1;
  }
  return out;
}

}  // namespace aadp::platform
