// The two machines whose numbers may be reported. Anything else is `other`, never reportable.
#pragma once

#include <string>
#include <string_view>

namespace aadp::platform {

inline constexpr std::string_view kHostMacM4Max = "mac-m4max";
inline constexpr std::string_view kHostLinuxZen1 = "linux-zen1";
inline constexpr std::string_view kHostOther = "other";

std::string_view classify_host(std::string_view os_name, std::string_view cpu_model);

// classify_host applied to this machine.
std::string detect_host_label();

}  // namespace aadp::platform
