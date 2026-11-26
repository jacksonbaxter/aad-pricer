// Which commit a run came from, taken when it runs, because a stale build must not be able to
// misreport it. A git checkout answers with git. An export from debian_sync has no .git, only
// the COMMIT file written into its root.
#ifndef AADP_RESULTS_PROVENANCE_HPP_
#define AADP_RESULTS_PROVENANCE_HPP_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aadp::results {

struct Provenance {
  std::string commit = "unknown";
  std::string source = "none";         // "git" or "COMMIT file"
  std::string tree_state = "unknown";  // "clean", "dirty", "exported" or "unknown"
};

Provenance read_provenance(const std::filesystem::path& root);

// Contents of a COMMIT file, or nullopt unless it holds one full hash.
std::optional<std::string> parse_commit_file(std::string_view text);

// Git's repository-location variables, the names `git rev-parse --local-env-vars` prints:
// GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE and the rest. Git exports them to whatever it runs from
// a hook or from `git rebase --exec`, and inherited they point `git -C <root>` at the caller's
// repository. Every git that read_provenance starts runs without them. Asked of git once per
// process, so no copy of the list is kept; nullopt if git cannot be run.
const std::optional<std::vector<std::string>>& git_location_variables();

}  // namespace aadp::results

#endif  // AADP_RESULTS_PROVENANCE_HPP_
