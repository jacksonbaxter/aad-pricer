#include "platform/cpu_info.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#ifdef __linux__
#include <sched.h>

#include <fstream>
#include <sstream>
#endif

namespace aadp::platform {
namespace {

std::string_view trim(std::string_view text) {
  constexpr std::string_view kSpace = " \t\r\n";
  const auto first = text.find_first_not_of(kSpace);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

// Calls fn(key, value) for each `key : value` line until it returns true.
template <typename Fn>
void for_each_colon_line(std::string_view text, Fn fn) {
  while (!text.empty()) {
    const auto end = text.find('\n');
    const std::string_view line = text.substr(0, end);
    const auto colon = line.find(':');
    if (colon != std::string_view::npos &&
        fn(trim(line.substr(0, colon)), trim(line.substr(colon + 1)))) {
      return;
    }
    if (end == std::string_view::npos) {
      return;
    }
    text.remove_prefix(end + 1);
  }
}

std::string lower(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

#ifdef __APPLE__
std::string sysctl_string(const char* name) {
  std::size_t size = 0;
  if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
    return {};
  }
  std::string value(size, '\0');
  if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
    return {};
  }
  value.resize(std::min(size, value.find('\0')));
  return value;
}
#endif

#ifdef __linux__
std::string read_first_line(const std::string& path) {
  std::ifstream in(path);
  std::string line;
  if (!in || !std::getline(in, line)) {
    return {};
  }
  return line;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}
#endif

}  // namespace

std::string short_hostname() {
  std::array<char, 256> name{};
  if (gethostname(name.data(), name.size() - 1) != 0) {
    return {};
  }
  const std::string_view view(name.data());
  return std::string(view.substr(0, view.find('.')));
}

std::string parse_cpuinfo_model(std::string_view cpuinfo) {
  std::string model;
  std::string implementer;
  std::string part;
  for_each_colon_line(cpuinfo, [&](std::string_view key, std::string_view value) {
    if (key == "model name" && !value.empty()) {
      model = std::string(value);
      return true;
    }
    if (key == "CPU implementer" && implementer.empty()) {
      implementer = std::string(value);
    } else if (key == "CPU part" && part.empty()) {
      part = std::string(value);
    }
    return false;
  });
  if (!model.empty()) {
    return model;
  }
  if (!implementer.empty() && !part.empty()) {
    return "arm64 implementer " + implementer + " part " + part;
  }
  return {};
}

std::optional<std::string> describe_midr(std::string_view midr_text) {
  const std::string_view text = trim(midr_text);
  if (text.size() < 3 || !text.starts_with("0x") ||
      !std::ranges::all_of(text.substr(2), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
    return std::nullopt;
  }
  return "arm64 midr_el1 " + std::string(text);
}

std::string parse_os_release_field(std::string_view os_release, std::string_view key) {
  while (!os_release.empty()) {
    const auto end = os_release.find('\n');
    const std::string_view line = os_release.substr(0, end);
    const auto equals = line.find('=');
    if (equals != std::string_view::npos && line.substr(0, equals) == key) {
      std::string_view value = trim(line.substr(equals + 1));
      if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
          value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
      }
      return std::string(value);
    }
    if (end == std::string_view::npos) {
      break;
    }
    os_release.remove_prefix(end + 1);
  }
  return {};
}

std::string describe_linux_os(std::string_view pretty_name, std::string_view version_id,
                              std::string_view hostname) {
  if (pretty_name.empty()) {
    return "Linux";
  }
  if (!hostname.empty() && lower(pretty_name).contains(lower(hostname))) {
    return "Linux, os-release VERSION_ID " +
           (version_id.empty() ? std::string("unknown") : std::string(version_id)) +
           ", name withheld";
  }
  return std::string(pretty_name);
}

CpuInfo read_cpu_info() {
  CpuInfo info;
  info.logical_cpus = static_cast<int>(std::thread::hardware_concurrency());
  utsname names{};
  if (uname(&names) == 0) {
    info.kernel = std::string(names.sysname) + " " + names.release;
    info.arch = names.machine;
  }
#ifdef __APPLE__
  info.os_name = "macOS";
  info.os = "macOS " + sysctl_string("kern.osproductversion") + " (build " +
            sysctl_string("kern.osversion") + ")";
  info.cpu_model = sysctl_string("machdep.cpu.brand_string");
#elifdef __linux__
  info.os_name = "Linux";
  const std::string os_release = read_file("/etc/os-release");
  info.os = describe_linux_os(parse_os_release_field(os_release, "PRETTY_NAME"),
                              parse_os_release_field(os_release, "VERSION_ID"), short_hostname());
  info.cpu_model = parse_cpuinfo_model(read_file("/proc/cpuinfo"));
  if (info.cpu_model.empty()) {
    // Reads the CPU this thread is on, because heterogeneous arm64 CPUs differ.
    const int cpu = std::max(sched_getcpu(), 0);
    const auto midr = describe_midr(read_first_line(
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/regs/identification/midr_el1"));
    if (midr) {
      info.cpu_model = *midr;
    }
  }
#else
#error "Only macOS and Linux are supported"
#endif
  if (info.cpu_model.empty()) {
    info.cpu_model = "unknown";
  }
  return info;
}

FrequencyInfo read_frequency_info(int cpu) {
  FrequencyInfo info;
#ifdef __linux__
  const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
  const auto set = [](std::string& field, const std::string& value) {
    if (!value.empty()) {
      field = value;
    }
  };
  set(info.governor, read_first_line(base + "/cpufreq/scaling_governor"));
  set(info.driver, read_first_line(base + "/cpufreq/scaling_driver"));
  const std::string now = read_first_line(base + "/cpufreq/scaling_cur_freq");
  if (!now.empty()) {
    info.frequency_khz = "cpu" + std::to_string(cpu) + ":" + now;
  }
  set(info.max_frequency_khz, read_first_line(base + "/cpufreq/cpuinfo_max_freq"));
  set(info.smt_siblings, read_first_line(base + "/topology/thread_siblings_list"));

  const std::string boost = read_first_line("/sys/devices/system/cpu/cpufreq/boost");
  const std::string no_turbo = read_first_line("/sys/devices/system/cpu/intel_pstate/no_turbo");
  if (boost == "1" || boost == "0") {
    info.turbo = boost == "1" ? "enabled" : "disabled";
  } else if (no_turbo == "1" || no_turbo == "0") {
    info.turbo = no_turbo == "0" ? "enabled" : "disabled";
  }
#else
  static_cast<void>(cpu);
#endif
  return info;
}

}  // namespace aadp::platform
