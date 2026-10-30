// Hardware counters. Linux: perf_event_open, user space only, cycles and instructions for this
// thread. macOS: proc_pid_rusage, which is process-wide and is labelled so. A counter that
// cannot be read stays nullopt with the reason in `note`; nothing stands in for it.
//
// Counts are raw. When the kernel multiplexed the group, time_running is below time_enabled and
// the report says so; scaling the counts up would make them an estimate.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace aadp::platform {

struct PerfCounterReport {
  std::string source;
  std::string scope;
  std::optional<std::uint64_t> cycles;
  std::optional<std::uint64_t> instructions;
  std::uint64_t time_enabled_ns = 0;
  std::uint64_t time_running_ns = 0;
  std::string note;
};

// Adds one region to a running total. A counter missing from either side is missing from the
// total, because a sum over some of the regions is not the number it looks like.
void accumulate(PerfCounterReport& total, const PerfCounterReport& region);

// The note for the results header.
std::string describe_counters(const PerfCounterReport& report);

class PerfCounterSession {
 public:
  PerfCounterSession();
  ~PerfCounterSession();
  PerfCounterSession(const PerfCounterSession&) = delete;
  PerfCounterSession& operator=(const PerfCounterSession&) = delete;

  void start();
  // Stops counting and reports what happened since start().
  PerfCounterReport stop();

 private:
  std::string open_error_;
  bool started_ = false;
#if defined(__linux__)
  int leader_fd_ = -1;
  int instructions_fd_ = -1;
#else
  std::uint64_t start_cycles_ = 0;
  std::uint64_t start_instructions_ = 0;
#endif
};

}  // namespace aadp::platform
