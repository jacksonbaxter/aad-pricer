"""Removes git's repository-location variables from this process before it starts git.

    import git_environment
    git_environment.clear()

Git exports GIT_DIR, and from a hook GIT_INDEX_FILE and others, to every command it runs from a
hook or from `git rebase --exec`. Inherited, they point `git -C <dir>` at the caller's repository
instead of <dir>: a test's scratch `init` and `commit` would land in the caller's history, and a
check would read the caller's files. The names are the ones `git rev-parse --local-env-vars`
prints, asked of git at run time, so no copy of the list is kept. Cleared from os.environ, so
every child started afterwards goes without them too. scripts/git_environment.sh does the same in
shell, and src/results/provenance.cpp in C++. Standard library only.
"""

import os
import subprocess
import sys


def location_variables():
    """The names git prints, or None if git cannot be started: then no git can mislead."""
    try:
        done = subprocess.run(
            ["git", "rev-parse", "--local-env-vars"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )  # fmt: skip
    except OSError:
        return None
    if done.returncode != 0:
        print(
            "git_environment: git rev-parse --local-env-vars failed: "
            + done.stderr.decode(errors="replace").strip(),
            file=sys.stderr,
        )
        raise SystemExit(2)
    return done.stdout.decode().split()


def clear():
    for name in location_variables() or ():
        os.environ.pop(name, None)
