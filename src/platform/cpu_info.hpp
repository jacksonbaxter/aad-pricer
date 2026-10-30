// What the machine is, for the results header. Nothing here may return the node name of the
// machine except short_hostname(), which exists so that the privacy scan can look for it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aadp::platform {

struct CpuInfo {
  std::string os_name;  // "macOS" or "Linux"
  std::string os;       // Product and version
  std::string kernel;   // sysname and release, never the node name
  std::string arch;     // uname machine
  std::string cpu_model;
  int logical_cpus = 0;
};

CpuInfo read_cpu_info();

// Governor, frequency and turbo for one CPU. Fields that do not exist here say "unavailable".
struct FrequencyInfo {
  std::string governor = "unavailable";
  std::string driver = "unavailable";
  std::string frequency_khz = "unavailable";
  std::string max_frequency_khz = "unavailable";
  std::string turbo = "unavailable";
  std::string smt_siblings = "unavailable";
};

FrequencyInfo read_frequency_info(int cpu);

// Node name without its domain, as `hostname -s` prints it.
std::string short_hostname();

// Value of `model name` in /proc/cpuinfo text, or of the implementer and part lines that arm64
// kernels give instead. Empty if neither exists.
std::string parse_cpuinfo_model(std::string_view cpuinfo);

// Formats the contents of a midr_el1 sysfs file, the last resort on arm64 Linux.
std::optional<std::string> describe_midr(std::string_view midr_text);

// One value from os-release text, with quotes removed. Empty if absent.
std::string parse_os_release_field(std::string_view os_release, std::string_view key);

// The header's os field for Linux. A distribution can share its name with the host, as
// debian's does, and the privacy scan must then find nothing, so the name is withheld and the
// version kept. Both the host name and the distribution are compared without case.
std::string describe_linux_os(std::string_view pretty_name, std::string_view version_id,
                              std::string_view hostname);

}  // namespace aadp::platform
