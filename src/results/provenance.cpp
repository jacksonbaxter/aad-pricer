#include "results/provenance.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "results/results_file.hpp"

namespace aadp::results {
namespace {

std::string shell_quoted(const std::string& text) {
  std::string out = "'";
  for (const char c : text) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  return out + "'";
}

std::optional<std::string> run_git(const std::filesystem::path& root, const std::string& args) {
  const std::string command = "git -C " + shell_quoted(root.string()) + " " + args + " 2>/dev/null";
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return std::nullopt;
  }
  std::string output;
  std::array<char, 4096> buffer{};
  std::size_t got = 0;
  while ((got = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
    output.append(buffer.data(), got);
  }
  if (pclose(pipe) != 0) {
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
    const auto head = run_git(root, "rev-parse HEAD");
    const auto status = run_git(root, "status --porcelain");
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
  std::ifstream in(root / "COMMIT");
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
