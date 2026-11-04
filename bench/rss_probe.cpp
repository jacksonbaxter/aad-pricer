#include "rss_probe.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <vector>

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX declares it nowhere.

namespace aadp::bench {

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

  posix_spawn_file_actions_t actions{};
  if (posix_spawn_file_actions_init(&actions) != 0) {
    return std::unexpected("posix_spawn_file_actions_init failed");
  }
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  pid_t pid = 0;
  const int spawned =
      posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) {
    return std::unexpected(std::string("posix_spawn failed: ") + std::strerror(spawned));
  }
  int status = 0;
  rusage usage{};
  pid_t waited = 0;
  do {
    waited = wait4(pid, &status, 0, &usage);
  } while (waited == -1 && errno == EINTR);
  if (waited != pid) return std::unexpected("wait4 failed");
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return std::unexpected("the probe did not exit cleanly");
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): glibc declares it inside a union.
  const long peak = usage.ru_maxrss;
  if (peak <= 0) return std::unexpected("no peak RSS reported");
  const auto value = static_cast<std::uint64_t>(peak);
#ifdef __APPLE__
  return value;  // Bytes on macOS
#else
  return value * 1024;  // KiB on Linux
#endif
}

}  // namespace aadp::bench
