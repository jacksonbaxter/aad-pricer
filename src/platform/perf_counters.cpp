#include "platform/perf_counters.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <system_error>

#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace aadp::platform {

void accumulate(PerfCounterReport& total, const PerfCounterReport& region) {
  if (total.source.empty()) {
    total = region;
    return;
  }
  const auto add = [](std::optional<std::uint64_t>& sum, std::optional<std::uint64_t> value) {
    sum = sum && value ? std::optional(*sum + *value) : std::nullopt;
  };
  add(total.cycles, region.cycles);
  add(total.instructions, region.instructions);
  total.time_enabled_ns += region.time_enabled_ns;
  total.time_running_ns += region.time_running_ns;
  if (total.note.empty()) {
    total.note = region.note;
  }
}

std::string describe_counters(const PerfCounterReport& report) {
  std::string text = report.note;
  if (report.time_running_ns != 0) {
    text += text.empty() ? "" : "; ";
    text +=
        "time_enabled_ns=" + std::to_string(report.time_enabled_ns) +
        " time_running_ns=" + std::to_string(report.time_running_ns) +
        (report.time_running_ns == report.time_enabled_ns ? " not multiplexed"
                                                          : " MULTIPLEXED, raw counts not scaled");
  }
  return text.empty() ? "none" : text;
}

#ifdef __linux__
namespace {

constexpr const char* kSource = "perf_event_open";
constexpr const char* kScope =
    "user space only, this thread (exclude_kernel, exclude_hv), measured repetitions only";

int open_counter(std::uint64_t config, int group_fd) {
  perf_event_attr attr{};
  attr.size = sizeof(attr);
  attr.type = PERF_TYPE_HARDWARE;
  attr.config = config;
  attr.disabled = group_fd == -1 ? 1 : 0;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.read_format =
      PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
  return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, group_fd, 0UL));
}

}  // namespace

PerfCounterSession::PerfCounterSession() {
  leader_fd_ = open_counter(PERF_COUNT_HW_CPU_CYCLES, -1);
  if (leader_fd_ < 0) {
    open_error_ = std::string("perf_event_open cycles: ") + std::generic_category().message(errno);
    return;
  }
  instructions_fd_ = open_counter(PERF_COUNT_HW_INSTRUCTIONS, leader_fd_);
  if (instructions_fd_ < 0) {
    open_error_ =
        std::string("perf_event_open instructions: ") + std::generic_category().message(errno);
  }
}

PerfCounterSession::~PerfCounterSession() {
  if (instructions_fd_ >= 0) {
    close(instructions_fd_);
  }
  if (leader_fd_ >= 0) {
    close(leader_fd_);
  }
}

void PerfCounterSession::start() {
  started_ = true;
  if (leader_fd_ < 0 || instructions_fd_ < 0) {
    return;
  }
  ioctl(leader_fd_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
  std::uint64_t data[5] = {};
  if (read(leader_fd_, data, sizeof(data)) == static_cast<ssize_t>(sizeof(data))) {
    start_enabled_ns_ = data[1];
    start_running_ns_ = data[2];
  }
  ioctl(leader_fd_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

PerfCounterReport PerfCounterSession::stop() {
  PerfCounterReport report;
  report.source = kSource;
  report.scope = kScope;
  if (!open_error_.empty()) {
    report.note = open_error_;
    return report;
  }
  if (!started_) {
    report.note = "stop() without start()";
    return report;
  }
  ioctl(leader_fd_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
  // Layout for PERF_FORMAT_GROUP with both time fields: nr, enabled, running, one value each.
  std::uint64_t data[5] = {};
  const ssize_t got = read(leader_fd_, data, sizeof(data));
  if (got != static_cast<ssize_t>(sizeof(data)) || data[0] != 2) {
    report.note = "short read from the counter group";
    return report;
  }
  report.time_enabled_ns = data[1] - start_enabled_ns_;
  report.time_running_ns = data[2] - start_running_ns_;
  if (report.time_running_ns == 0) {
    report.note = "counters never ran (time_running is 0)";
    return report;
  }
  report.cycles = data[3];
  report.instructions = data[4];
  return report;
}

#elifdef __APPLE__

namespace {

constexpr const char* kSource = "proc_pid_rusage";
constexpr const char* kScope =
    "process-wide, user and kernel, all threads, measured repetitions only";

bool read_rusage(rusage_info_v4& info) {
  // The SDK types this buffer as void**, and the call writes a rusage_info_v4 through it.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): see above.
  return proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) == 0;
}

}  // namespace

PerfCounterSession::PerfCounterSession() = default;
PerfCounterSession::~PerfCounterSession() = default;

void PerfCounterSession::start() {
  rusage_info_v4 info{};
  if (!read_rusage(info)) {
    open_error_ = std::string("proc_pid_rusage: ") + std::generic_category().message(errno);
    return;
  }
  start_cycles_ = info.ri_cycles;
  start_instructions_ = info.ri_instructions;
  started_ = true;
}

PerfCounterReport PerfCounterSession::stop() {
  PerfCounterReport report;
  report.source = kSource;
  report.scope = kScope;
  if (!started_) {
    report.note = open_error_.empty() ? "stop() without start()" : open_error_;
    return report;
  }
  rusage_info_v4 info{};
  if (!read_rusage(info)) {
    report.note = std::string("proc_pid_rusage: ") + std::generic_category().message(errno);
    return report;
  }
  // A kernel that does not fill these leaves them at zero, which is missing, not a count.
  if (info.ri_cycles > start_cycles_) {
    report.cycles = info.ri_cycles - start_cycles_;
  }
  if (info.ri_instructions > start_instructions_) {
    report.instructions = info.ri_instructions - start_instructions_;
  }
  if (!report.cycles || !report.instructions) {
    report.note = "ri_cycles or ri_instructions did not move";
  }
  return report;
}

#else
#error "Only macOS and Linux are supported"
#endif

}  // namespace aadp::platform
