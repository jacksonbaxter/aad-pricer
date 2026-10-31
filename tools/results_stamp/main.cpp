// Turns a binary's raw output and the stderr captured around it into a results file. The rules
// are in results/stamp.hpp; this file does the reading and the writing.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "platform/cpu_info.hpp"
#include "platform/host_label.hpp"
#include "results/stamp.hpp"

namespace {

constexpr int kExitRefused = 1;
constexpr int kExitUsage = 2;

constexpr std::string_view kUsage =
    "usage: aadp_results_stamp --raw=<file> --stderr=<file> --out=<file>\n"
    "                          (--reportable | --not-reportable=<reason>)\n"
    "                          [--forbid=<text>]... \n"
    "       aadp_results_stamp --print-host-label\n";

std::optional<std::string> read_file(const std::string& path) {
  const std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// Through a temporary name, so that a reader never sees half a file.
bool write_file(const std::string& path, std::string_view text) {
  const std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary);
    out << text;
    out.flush();
    if (!out) {
      return false;
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  return !error;
}

}  // namespace

int main(int argc, char* const* argv) try {
  std::string raw_path;
  std::string stderr_path;
  std::string out_path;
  aadp::results::StampRequest request;
  bool mode_set = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value_of = [arg](std::string_view flag) -> std::optional<std::string_view> {
      return arg.starts_with(flag) ? std::optional(arg.substr(flag.size())) : std::nullopt;
    };
    if (arg == "--print-host-label") {
      std::cout << aadp::platform::detect_host_label() << "\n";
      return 0;
    }
    if (const auto raw = value_of("--raw=")) {
      raw_path = *raw;
    } else if (const auto captured = value_of("--stderr=")) {
      stderr_path = *captured;
    } else if (const auto out = value_of("--out=")) {
      out_path = *out;
    } else if (const auto forbid = value_of("--forbid=")) {
      request.privacy.forbidden.emplace_back(*forbid);
    } else if (arg == "--reportable") {
      request.reportable = true;
      mode_set = true;
    } else if (const auto reason = value_of("--not-reportable=")) {
      request.reportable = false;
      request.not_reportable_reason = *reason;
      mode_set = true;
    } else {
      std::cerr << kUsage;
      return kExitUsage;
    }
  }
  if (raw_path.empty() || stderr_path.empty() || out_path.empty() || !mode_set) {
    std::cerr << kUsage;
    return kExitUsage;
  }

  const auto raw = read_file(raw_path);
  if (!raw) {
    std::cerr << "results_stamp: cannot read the raw output\n";
    return kExitRefused;
  }
  // A missing capture is what a run outside measure.sh looks like, so it is empty, not an error.
  const std::string captured = read_file(stderr_path).value_or("");

  request.raw_output = *raw;
  request.captured_stderr = captured;
  request.current_host_label = aadp::platform::detect_host_label();
  request.current_hostname = aadp::platform::short_hostname();
  // NOLINTBEGIN(concurrency-mt-unsafe): this tool has one thread and nothing calls setenv.
  if (const char* home = std::getenv("HOME"); home != nullptr) {
    request.privacy.forbidden.emplace_back(home);
  }
  if (const char* user = std::getenv("USER"); user != nullptr) {
    request.privacy.forbidden.emplace_back(user);
  }
  // NOLINTEND(concurrency-mt-unsafe)

  auto stamped = aadp::results::stamp_results(request);
  if (!stamped) {
    std::cerr << "results_stamp: refused: " << stamped.error().detail << "\n";
    return kExitRefused;
  }
  if (!write_file(out_path, *stamped)) {
    std::cerr << "results_stamp: cannot write the output\n";
    return kExitRefused;
  }
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "results_stamp: %s\n", error.what());
  return kExitRefused;
}
