// Which commit a run came from, taken when it runs, because a stale build must not be able to
// misreport it. A git checkout answers with git. An export from debian_sync has no .git, only
// the COMMIT file written into its root.
#ifndef AADP_RESULTS_PROVENANCE_HPP_
#define AADP_RESULTS_PROVENANCE_HPP_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace aadp::results {

struct Provenance {
  std::string commit = "unknown";
  std::string source = "none";         // "git" or "COMMIT file"
  std::string tree_state = "unknown";  // "clean", "dirty", "exported" or "unknown"
};

Provenance read_provenance(const std::filesystem::path& root);

// Contents of a COMMIT file, or nullopt unless it holds one full hash.
std::optional<std::string> parse_commit_file(std::string_view text);

}  // namespace aadp::results

#endif  // AADP_RESULTS_PROVENANCE_HPP_
