// For a test that starts git itself, through std::system: its git commands must act on its
// scratch directory, never on a repository that a hook or `git rebase --exec` running the tests
// named in GIT_DIR, GIT_WORK_TREE or GIT_INDEX_FILE. read_provenance clears these for its own
// git; this clears them for the test's. scripts/git_environment.py and .sh do the same.
#ifndef AADP_TESTS_BENCH_GIT_ENVIRONMENT_HPP_
#define AADP_TESTS_BENCH_GIT_ENVIRONMENT_HPP_

#include <cstdlib>
#include <string>

#include "results/provenance.hpp"

namespace aadp::testing {

// Removes the names `git rev-parse --local-env-vars` prints from this process's environment.
inline void clear_git_location_variables() {
  if (const auto& names = results::git_location_variables()) {
    for (const std::string& name : *names) {
      unsetenv(name.c_str());
    }
  }
}

}  // namespace aadp::testing

#endif  // AADP_TESTS_BENCH_GIT_ENVIRONMENT_HPP_
