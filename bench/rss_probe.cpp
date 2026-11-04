#include "rss_probe.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef __linux__
#include <fstream>
#endif

// POSIX declares it nowhere, and posix_spawn takes it as it is.
// NOLINTNEXTLINE(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)
extern char** environ;

namespace aadp::bench {
namespace {

std::expected<std::uint64_t, std::string> OwnPeakBytes() {
#ifdef __linux__
  // "VmHWM:     4152 kB": the high-water RSS of the address space exec made.
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    constexpr std::string_view kKey = "VmHWM:";
    if (!line.starts_with(kKey)) continue;
    std::string_view rest = std::string_view(line).substr(kKey.size());
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    std::uint64_t kib = 0;
    const auto [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), kib);
    if (error != std::errc{} || std::string_view(end, rest.data() + rest.size()) != " kB") {
      return std::unexpected("cannot read VmHWM");
    }
    return kib * 1024;
  }
  return std::unexpected("no VmHWM in /proc/self/status");
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return std::unexpected("getrusage failed");
  const long peak = usage.ru_maxrss;  // Bytes on macOS
  if (peak <= 0) return std::unexpected("no peak RSS reported");
  return static_cast<std::uint64_t>(peak);
#endif
}

// Both ends close on exec; the child's stdout is a dup2 of the write end, which does not.
bool MakePipe(std::array<int, 2>& fds) {
  if (pipe(fds.data()) != 0) return false;
  for (const int fd : fds) {
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
      close(fds[0]);
      close(fds[1]);
      return false;
    }
  }
  return true;
}

}  // namespace

bool ReportOwnPeakRss() {
  const auto peak = OwnPeakBytes();
  if (!peak) return false;
  const std::string line = std::string(kPeakReportPrefix) + std::to_string(*peak) + "\n";
  return std::fputs(line.c_str(), stdout) >= 0 && std::fflush(stdout) == 0;
}

std::expected<std::uint64_t, std::string> ParsePeakReport(std::string_view output) {
  const auto at = output.rfind(kPeakReportPrefix);
  if (at == std::string_view::npos || (at != 0 && output[at - 1] != '\n')) {
    return std::unexpected("the probe reported no peak");
  }
  std::string_view value = output.substr(at + kPeakReportPrefix.size());
  value = value.substr(0, value.find('\n'));
  std::uint64_t bytes = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), bytes);
  if (error != std::errc{} || end != value.data() + value.size() || bytes == 0) {
    return std::unexpected("the probe's peak report does not parse");
  }
  return bytes;
}

std::expected<std::uint64_t, std::string> ChildPeakRss(const std::string& executable,
                                                       const std::vector<std::string>& args) {
  std::vector<std::string> storage;
  storage.reserve(args.size() + 1);
  storage.push_back(executable);
  storage.insert(storage.end(), args.begin(), args.end());
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& arg : storage) argv.push_back(arg.data());
  argv.push_back(nullptr);

  std::array<int, 2> fds{-1, -1};
  if (!MakePipe(fds)) return std::unexpected("pipe failed");
  posix_spawn_file_actions_t actions{};
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(fds[0]);
    close(fds[1]);
    return std::unexpected("posix_spawn_file_actions_init failed");
  }
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  pid_t pid = 0;
  const int spawned =
      posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (spawned != 0) {
    close(fds[0]);
    return std::unexpected("posix_spawn failed: " +
                           std::error_code(spawned, std::generic_category()).message());
  }

  std::string output;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t got = read(fds[0], buffer.data(), buffer.size());
    if (got > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(got));
    } else if (got == 0 || errno != EINTR) {
      break;
    }
  }
  close(fds[0]);

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  while (waited == -1 && errno == EINTR) waited = waitpid(pid, &status, 0);
  if (waited != pid) return std::unexpected("waitpid failed");
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return std::unexpected("the probe did not exit cleanly");
  }
  return ParsePeakReport(output);
}

}  // namespace aadp::bench
