#include "results/provenance.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "results/results_file.hpp"

// POSIX gives no header for this.
extern "C" char** environ;

namespace aadp::results {
namespace {

// Output of `git -C <root> <args>`, or nothing if git failed. Started directly, not through a
// shell, so the path needs no quoting.
std::optional<std::string> run_git(const std::filesystem::path& root,
                                   std::vector<std::string> arguments) {
  std::vector<std::string> words = {"git", "-C", root.string()};
  words.insert(words.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  argv.reserve(words.size() + 1);
  for (std::string& word : words) {
    argv.push_back(word.data());
  }
  argv.push_back(nullptr);

  std::array<int, 2> pipe_ends{};
  if (pipe(pipe_ends.data()) != 0) {
    return std::nullopt;
  }
  posix_spawn_file_actions_t actions{};
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
  posix_spawn_file_actions_addclose(&actions, pipe_ends[1]);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  pid_t child = 0;
  const int spawned = posix_spawnp(&child, "git", &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipe_ends[1]);
  if (spawned != 0) {
    close(pipe_ends[0]);
    return std::nullopt;
  }

  std::string output;
  std::array<char, 4096> buffer{};
  ssize_t got = 0;
  while ((got = read(pipe_ends[0], buffer.data(), buffer.size())) != 0) {
    if (got > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(got));
    } else if (errno != EINTR) {
      break;
    }
  }
  close(pipe_ends[0]);

  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      return std::nullopt;
    }
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return std::nullopt;
  }
  return output;
}

}  // namespace

std::optional<std::string> parse_commit_file(std::string_view text) {
  while (text.ends_with('\n') || text.ends_with('\r')) {
    text.remove_suffix(1);
  }
  if (!is_commit_hash(text)) {
    return std::nullopt;
  }
  return std::string(text);
}

Provenance read_provenance(const std::filesystem::path& root) {
  Provenance out;
  std::error_code ignored;
  // Tests for .git here rather than asking git, which would walk up and name an enclosing repo.
  if (std::filesystem::exists(root / ".git", ignored)) {
    const auto head = run_git(root, {"rev-parse", "HEAD"});
    const auto status = run_git(root, {"status", "--porcelain"});
    if (head) {
      if (const auto commit = parse_commit_file(*head)) {
        out.commit = *commit;
        out.source = "git";
      }
    }
    if (status) {
      out.tree_state = status->empty() ? "clean" : "dirty";
    }
    return out;
  }
  const std::ifstream in(root / "COMMIT");
  if (in) {
    std::ostringstream text;
    text << in.rdbuf();
    if (const auto commit = parse_commit_file(text.str())) {
      out.commit = *commit;
      out.source = "COMMIT file";
      out.tree_state = "exported";
    }
  }
  return out;
}

}  // namespace aadp::results
