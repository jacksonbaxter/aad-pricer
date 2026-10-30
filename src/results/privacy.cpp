#include "results/privacy.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace aadp::results {
namespace {

// The directory names are written apart from their slashes on purpose: this file must itself
// pass the repo's hygiene grep.
constexpr std::array<std::string_view, 4> kHomeRoots = {"users", "home", "root", "volumes"};

char to_lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string lowered(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), to_lower);
  return out;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f'); }
bool is_word(char c) { return is_digit(c) || (c >= 'a' && c <= 'z') || c == '_'; }

bool has_home_path(std::string_view line) {
  for (const std::string_view root : kHomeRoots) {
    const std::string needle = "/" + std::string(root) + "/";
    if (line.find(needle) != std::string_view::npos) {
      return true;
    }
    const std::string windows = ":\\" + std::string(root) + "\\";
    if (line.find(windows) != std::string_view::npos) {
      return true;
    }
  }
  // A tilde that opens a word is a home path; one inside a word, as in a compiler flag, is not.
  for (auto at = line.find("~/"); at != std::string_view::npos; at = line.find("~/", at + 1)) {
    if (at == 0 || !is_word(line[at - 1])) {
      return true;
    }
  }
  return false;
}

// Four dotted octets, each at most 255, not part of a longer dotted run such as a version.
bool has_ipv4(std::string_view line) {
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (!is_digit(line[i]) || (i > 0 && (is_word(line[i - 1]) || line[i - 1] == '.'))) {
      continue;
    }
    std::size_t at = i;
    int octets = 0;
    while (octets < 5) {
      std::size_t digits = 0;
      int value = 0;
      while (at < line.size() && is_digit(line[at]) && digits < 4) {
        value = value * 10 + (line[at] - '0');
        ++at;
        ++digits;
      }
      if (digits == 0 || digits > 3 || value > 255) {
        octets = -1;
        break;
      }
      ++octets;
      if (at >= line.size() || line[at] != '.' || at + 1 >= line.size() ||
          !is_digit(line[at + 1])) {
        break;
      }
      ++at;
    }
    if (octets == 4 && (at >= line.size() || !is_digit(line[at]))) {
      return true;
    }
  }
  return false;
}

// A token of hex digits and colons with `::`, or with all eight groups. Times and C++ names
// fail one of those tests.
bool has_ipv6(std::string_view line) {
  std::size_t i = 0;
  while (i < line.size()) {
    if (!is_hex(line[i]) && line[i] != ':') {
      ++i;
      continue;
    }
    std::size_t end = i;
    while (end < line.size() && (is_hex(line[end]) || line[end] == ':')) {
      ++end;
    }
    const std::string_view token = line.substr(i, end - i);
    const bool bounded =
        (i == 0 || !is_word(line[i - 1])) && (end == line.size() || !is_word(line[end]));
    const auto colons = std::ranges::count(token, ':');
    const bool compressed =
        token.find("::") != std::string_view::npos && token.size() > 2 && colons >= 2;
    if (bounded && (compressed || colons == 7)) {
      return true;
    }
    i = end;
  }
  return false;
}

}  // namespace

std::vector<PrivacyHit> scan_private_text(std::string_view text, const PrivacyConfig& config) {
  std::vector<std::string> hostnames;
  for (const std::string& name : config.hostnames) {
    if (name.empty()) {
      continue;
    }
    hostnames.push_back(lowered(name));
    const auto dot = hostnames.back().find('.');
    if (dot != std::string::npos && dot != 0) {
      hostnames.push_back(hostnames.back().substr(0, dot));
    }
  }
  std::vector<std::string> forbidden;
  for (const std::string& value : config.forbidden) {
    if (!value.empty()) {
      forbidden.push_back(lowered(value));
    }
  }

  std::vector<PrivacyHit> hits;
  std::size_t number = 0;
  while (!text.empty()) {
    const auto end = text.find('\n');
    const std::string line = lowered(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    ++number;
    if (has_home_path(line)) {
      hits.push_back({"home path", number});
    }
    if (has_ipv4(line) || has_ipv6(line)) {
      hits.push_back({"ip address", number});
    }
    const auto contains = [&line](const std::string& needle) {
      return line.find(needle) != std::string::npos;
    };
    // The field itself counts, whatever its value: a measure line copied from another machine
    // names a host this scan was never told about.
    if (line.find("host=") != std::string::npos || std::ranges::any_of(hostnames, contains)) {
      hits.push_back({"hostname", number});
    }
    // The stamp drops these keys from the benchmark JSON. Finding one now means it sits somewhere
    // the JSON filter never looked, and a relative path in it would pass every other check.
    if (line.find("host_name") != std::string::npos ||
        line.find("executable\"") != std::string::npos) {
      hits.push_back({"google benchmark private key", number});
    }
    if (std::ranges::any_of(forbidden, contains)) {
      hits.push_back({"forbidden string", number});
    }
  }
  return hits;
}

}  // namespace aadp::results
