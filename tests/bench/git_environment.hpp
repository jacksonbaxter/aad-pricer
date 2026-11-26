// For a test that starts git itself, through std::system: its git commands must act on its
// scratch directory, never on a repository that a hook or `git rebase --exec` running the tests
// named in GIT_DIR, GIT_WORK_TREE or GIT_INDEX_FILE. read_provenance clears these for its own
// git; this clears them for the test's. scripts/git_environment.py and .sh do the same.
#ifndef AADP_TESTS_BENCH_GIT_ENVIRONMENT_HPP_
#define AADP_TESTS_BENCH_GIT_ENVIRONMENT_HPP_

#include <algorithm>
#include <cstdlib>
#include <string>

#include "results/provenance.hpp"

namespace aadp::testing {

// Removes the names `git rev-parse --local-env-vars` prints from this process's environment.
// False when git gives no list (not found, or the command failed) or a name cannot be removed:
// the caller asserts it, so that a test that cannot clear the variables fails and says why,
// as the Python and shell helpers stop with exit 2, instead of going on with them set.
[[nodiscard]] inline bool clear_git_location_variables() {
  const auto& names = results::git_location_variables();
  return names && std::ranges::all_of(
                      *names, [](const std::string& name) { return unsetenv(name.c_str()) == 0; });
}

inline constexpr const char* kCannotClearGitLocationVariables =
    "cannot clear git's location variables: `git rev-parse --local-env-vars` gave no list, or a "
    "name it gave could not be unset";

}  // namespace aadp::testing

#endif  // AADP_TESTS_BENCH_GIT_ENVIRONMENT_HPP_
