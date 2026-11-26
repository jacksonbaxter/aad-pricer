#!/usr/bin/env python3
"""The tests and checks that start git, run as a hook or `git rebase --exec` would run them.

    python3 -I tests/bench/git_environment_test.py <scratch-dir> <harness selftest>
        <report selftest> <aadp_results_unit> <aadp_bench_cli>

Git exports GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE and others to every command it runs from a
hook or from rebase --exec. This makes a scratch "caller" repository of its own, under the
scratch directory and nowhere else, points those three variables at it, and runs each step with
them set: the two run_bench.sh script tests, the repo scan's and the commit verifier's tests, the
provenance and dirty-tree tests of the two C++ test programs, the fault runner's test of its git
field, the repo scan on a second scratch repository, and the harness reading the provenance of
that repository. After each step the caller must be as it was: the same HEAD, refs, config bytes,
index bytes, objects reachable from a ref and work tree. The scan and the harness must also name
the repository they were given, not the caller. Every step clears the variables itself, through
scripts/git_environment.py or .sh or read_provenance; this test does not clear them for it.

Run alone (RUN_SERIAL): the C++ tests write to fixed scratch directories of their own, and the
fault runner's test builds a small CMake project. Exit 0 when every check passes. Standard
library only.
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.dont_write_bytecode = True  # no scripts/__pycache__ in the tracked tree
sys.path.insert(0, os.path.join(SOURCE, "scripts"))
import git_environment  # noqa: E402

IDENTITY = ["-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false"]


def git(repo, *arguments):
    """git in a scratch repo of this test's, with the location variables cleared."""
    done = subprocess.run(
        ["git", "-C", repo, *IDENTITY, "-c", f"core.hooksPath={repo}/.no-hooks", *arguments],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )  # fmt: skip
    if done.returncode != 0:
        raise RuntimeError(f"git {' '.join(arguments)} in {repo}: {done.stderr.decode().strip()}")
    return done.stdout.decode()


def make_repo(path, files):
    os.makedirs(path)
    git(path, "init", "-q")
    for name, text in files.items():
        with open(os.path.join(path, name), "w", encoding="utf-8") as file:
            file.write(text)
    git(path, "add", "--", *files)
    git(path, "commit", "-q", "-m", "first")
    return git(path, "rev-parse", "HEAD").strip()


def read_bytes(path):
    with open(path, "rb") as file:
        return file.read()


def work_tree(root):
    """Every path under root but .git, with the bytes of each file."""
    found = {}
    for directory, dirs, files in os.walk(root):
        dirs[:] = sorted(name for name in dirs if not (directory == root and name == ".git"))
        for name in files:
            path = os.path.join(directory, name)
            found[os.path.relpath(path, root)] = read_bytes(path)
    return found


def snapshot(caller):
    """What a step must leave as it was, read with plumbing that writes nothing."""
    dot_git = os.path.join(caller, ".git")
    return {
        "HEAD": read_bytes(os.path.join(dot_git, "HEAD")).decode().strip()
        + " at "
        + git(caller, "rev-parse", "HEAD").strip(),
        "refs": git(caller, "for-each-ref", "--format=%(refname) %(objectname)"),
        "config bytes": read_bytes(os.path.join(dot_git, "config")),
        "index bytes": read_bytes(os.path.join(dot_git, "index")),
        "objects reachable from a ref": sorted(
            line.split()[0] for line in git(caller, "rev-list", "--objects", "--all").splitlines()
        ),
        "work tree": work_tree(caller),
    }


def changes(before, after):
    lines = []
    for key, old in before.items():
        new = after[key]
        if new == old:
            continue
        if key in ("HEAD", "refs"):
            lines.append(f"{key} {old.strip()!r} -> {new.strip()!r}")
        elif key == "objects reachable from a ref":
            lines.append(f"{key}: {len(set(new) - set(old))} new, {len(set(old) - set(new))} gone")
        elif key == "work tree":
            added = sorted(set(new) - set(old))
            altered = sorted(name for name in old if name in new and new[name] != old[name])
            lines.append(f"{key}: added {added[:5]}, changed {altered[:5]}")
        else:
            lines.append(f"{key}: {len(old)} bytes -> {len(new)} bytes")
    return lines


def header_value(path, key):
    with open(path, encoding="utf-8", errors="replace") as file:
        for line in file:
            if line.startswith(key + ": "):
                return line[len(key) + 2 :].strip()
    return None


def main():
    if len(sys.argv) != 6:
        print(
            "usage: git_environment_test.py <scratch-dir> <harness selftest> <report selftest> "
            "<aadp_results_unit> <aadp_bench_cli>",
            file=sys.stderr,
        )
        return 2
    scratch = os.path.realpath(sys.argv[1])
    harness, report, results_unit, bench_cli = (os.path.abspath(path) for path in sys.argv[2:])
    # This test's own git commands must not follow a caller of its own either.
    git_environment.clear()
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch)

    caller = os.path.join(scratch, "caller")
    make_repo(caller, {"caller.txt": "the caller's file\n"})
    # Untracked, so that a status read in the caller says dirty.
    with open(os.path.join(caller, "untracked.txt"), "w", encoding="utf-8") as file:
        file.write("not tracked\n")
    home = os.path.join(scratch, "home")
    home_head = make_repo(home, {"one.txt": "one\n", "two.txt": "two\n"})

    environment = dict(
        os.environ,
        GIT_DIR=os.path.join(caller, ".git"),
        GIT_WORK_TREE=caller,
        GIT_INDEX_FILE=os.path.join(caller, ".git", "index"),
    )
    python = [sys.executable, "-I"]
    raw = os.path.join(scratch, "harness-raw.txt")
    git_field = (
        "ChecksAsTheyReallyRun."
        "test_the_git_field_gives_a_tree_of_its_own_and_leaves_the_real_index"
    )
    steps = [
        ("run_bench_test.sh", ["bash", "tests/bench/run_bench_test.sh", harness,
                               os.path.join(scratch, "run_bench")], None),
        ("run_bench_report_test.sh", ["bash", "tests/bench/run_bench_report_test.sh", report,
                                      os.path.join(scratch, "run_bench_report")], None),
        ("scan_repo_test.py", [*python, "tests/data/scan_repo_test.py",
                               os.path.join(scratch, "scan_repo")], None),
        ("verify_commits_test.py", [*python, "tests/data/verify_commits_test.py",
                                    os.path.join(scratch, "verify_commits")], None),
        ("aadp_results_unit Provenance", [results_unit, "--gtest_filter=Provenance.*"], None),
        ("aadp_bench_cli dirty tree",
         [bench_cli, "--gtest_filter=HarnessSelftest.ReportableRefusesADirtyTree"], None),
        ("test_fault_injection.py git field",
         [*python, "faults/test_fault_injection.py", git_field], None),
        ("scan_repo.py --root home", [*python, "scripts/scan_repo.py", "--root", home], None),
        ("harness provenance of home",
         [harness, f"--source-root={home}", "--batches=100", "--repetitions=2", "--warmup-ms=1",
          "--gb-warmup-s=0.001"], raw),
    ]  # fmt: skip

    failures = 0
    passed = 0

    def report_line(ok, text):
        nonlocal failures, passed
        print(("ok     " if ok else "FAILED ") + text)
        if ok:
            passed += 1
        else:
            failures += 1

    for name, command, stdout_path in steps:
        before = snapshot(caller)
        if stdout_path is None:
            done = subprocess.run(
                command, cwd=SOURCE, env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False,
            )  # fmt: skip
            output = done.stdout.decode(errors="replace")
        else:
            with open(stdout_path, "wb") as out:
                done = subprocess.run(
                    command, cwd=SOURCE, env=environment, stdout=out, stderr=subprocess.PIPE,
                    check=False,
                )  # fmt: skip
            output = done.stderr.decode(errors="replace")
        report_line(done.returncode == 0, f"{name}: exit {done.returncode}")
        if done.returncode != 0:
            for line in output.splitlines()[-15:]:
                print("       " + line)
        changed = changes(before, snapshot(caller))
        report_line(not changed, f"{name}: the caller is untouched")
        for line in changed:
            print(f"       {name} changed the caller: {line}")
        if name == "scan_repo.py --root home":
            wanted = "scan_repo: 2 tracked files, 0 missing from the working tree"
            report_line(wanted in output, f"{name}: it scans home's two files")
        if name == "harness provenance of home":
            commit = header_value(raw, "commit")
            tree = header_value(raw, "tree_state")
            report_line(commit == home_head, f"{name}: commit {commit}, home is {home_head}")
            report_line(tree == "clean", f"{name}: tree_state {tree}, home is clean")

    print(f"git_environment_test: {passed} of {passed + failures} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
