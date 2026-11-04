// What the machine is, for the results header. Nothing here may return the node name of the
// machine except short_hostname(), which exists so that the privacy scan can look for it.
#ifndef AADP_PLATFORM_CPU_INFO_HPP_
#define AADP_PLATFORM_CPU_INFO_HPP_

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

// x86 CPUID family and model, extended fields included (AMD and Intel: the extended family is
// added when the base family is 0xF, the extended model is the high nibble). nullopt off x86.
struct X86Model {
  unsigned family = 0;
  unsigned model = 0;
};
std::optional<X86Model> read_x86_model();

// What every lane ceiling on this CPU must carry beside it. AMD family 17h below model 30h (Zen
// and Zen+, such as the Raven Ridge V1605B, 17h model 11h) executes each 256-bit AVX operation as
// two 128-bit halves on a 128-bit FP datapath, so 4 lanes of double give about 2 lanes'
// throughput there: "Zen 1: 128-bit FP datapath". Zen 2 (17h, 30h on) widened it to 256 bits.
// Empty for every other CPU, and off x86.
std::string_view fp_datapath_note(std::optional<X86Model> cpu);

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

#endif  // AADP_PLATFORM_CPU_INFO_HPP_
