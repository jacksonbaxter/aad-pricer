// The last check before a results file is written. It reports what kind of thing it found and
// on which line, never the text, so that the refusal message is safe to log.
#ifndef AADP_RESULTS_PRIVACY_HPP_
#define AADP_RESULTS_PRIVACY_HPP_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace aadp::results {

struct PrivacyConfig {
  std::vector<std::string> hostnames;  // Each is also searched without its domain
  std::vector<std::string> forbidden;  // Any other strings, such as a user name or $HOME
};

struct PrivacyHit {
  std::string_view kind;  // "home path", "ip address", "hostname", "forbidden string", ...
  std::size_t line = 0;   // From 1
};

// All comparisons ignore case: host names do, and so do default macOS paths.
std::vector<PrivacyHit> scan_private_text(std::string_view text, const PrivacyConfig& config);

}  // namespace aadp::results

#endif  // AADP_RESULTS_PRIVACY_HPP_
