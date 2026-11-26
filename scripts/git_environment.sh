# shellcheck shell=bash
# Sourced, not run: removes git's repository-location variables from the shell that sources it,
# before it starts git.
#
#   . "<this repository>/scripts/git_environment.sh"
#
# Git exports GIT_DIR, and from a hook GIT_INDEX_FILE and others, to every command it runs from a
# hook or from `git rebase --exec`. Inherited, they point `git -C <dir>` at the caller's
# repository instead of <dir>. The names are the ones `git rev-parse --local-env-vars` prints,
# asked of git at run time, so no copy of the list is kept. Unset, so every child started
# afterwards goes without them too. scripts/git_environment.py does the same in Python. With no
# git on PATH it does nothing, as no git can then be misled. Runs on the bash 3.2 that macOS ships.
if command -v git >/dev/null 2>&1; then
  if ! aadp_git_location_variables=$(git rev-parse --local-env-vars); then
    printf 'git_environment: git rev-parse --local-env-vars failed\n' >&2
    exit 2
  fi
  # shellcheck disable=SC2086 # One name per word, and no name has a space or a glob in it.
  unset $aadp_git_location_variables
  unset aadp_git_location_variables
fi
