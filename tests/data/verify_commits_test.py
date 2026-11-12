#!/usr/bin/env python3
"""The tree sync of scripts/verify_commits.sh: scripts/sync_tree.py alone, then the verifier
itself on a scratch repo, with a PATH that has no rsync.

    python3 -I tests/data/verify_commits_test.py <scratch-dir>

The scratch directory is emptied. The verifier's commits have a CMakeLists.txt but no presets, so
each one is exported and synced, and then fails to configure, which is quick. Needs on PATH what
the verifier needs: cmake, ctest, git, ninja, tar and the usual shell tools. Exit 0 when every case
gives its expected result. Standard library only.
"""
import os
import shutil
import stat
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.normpath(os.path.join(HERE, "..", "..", "scripts"))
SYNC = os.path.join(SCRIPTS, "sync_tree.py")
VERIFY = os.path.join(SCRIPTS, "verify_commits.sh")

# 2001-09-09: older than anything a run writes, so a rewritten file cannot keep it by chance.
OLD = 1_000_000_000
OLD_NS = OLD * 1_000_000_000

# What verify_commits.sh runs, rsync not among them.
TOOLS = [
    "bash", "cmake", "ctest", "dirname", "git", "grep", "ln", "mkdir", "ninja", "ps", "rm",
    "tail", "tar", "tee", "tr",
]  # fmt: skip


def write(path, content, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as file:
        file.write(content)
    os.chmod(path, mode)


def read(path):
    with open(path, encoding="utf-8") as file:
        return file.read()


def content(path):
    return read(path) if os.path.isfile(path) else None


def age(root):
    """Sets every file, link and directory under root to OLD, without following links."""
    for directory, dirs, files in os.walk(root):
        for name in dirs + files:
            os.utime(os.path.join(directory, name), (OLD, OLD), follow_symlinks=False)


def mtime_ns(path):
    return os.lstat(path).st_mtime_ns if os.path.lexists(path) else None


def listing(root, skip_top=()):
    """Every path under root with its kind, mode and content or link target."""
    entries = {}
    for directory, dirs, files in os.walk(root):
        rel_dir = os.path.relpath(directory, root)
        if rel_dir == ".":
            dirs[:] = [name for name in dirs if name not in skip_top]
            files = [name for name in files if name not in skip_top]
        for name in sorted(dirs + files):
            path = os.path.join(directory, name)
            rel = os.path.normpath(os.path.join(rel_dir, name))
            info = os.lstat(path)
            mode = stat.S_IMODE(info.st_mode)
            if stat.S_ISLNK(info.st_mode):
                entries[rel] = ("link", os.readlink(path))
            elif stat.S_ISDIR(info.st_mode):
                entries[rel] = ("dir", mode)
            else:
                entries[rel] = ("file", mode, read(path))
        dirs[:] = [name for name in dirs if not os.path.islink(os.path.join(directory, name))]
    return entries


def run_sync(*arguments):
    done = subprocess.run(
        [sys.executable, "-I", SYNC, *arguments], capture_output=True, text=True, check=False
    )
    return done.returncode, done.stdout + done.stderr


def sync_cases(scratch):
    """sync_tree.py against a destination that differs from its source in every way it can."""
    source = os.path.join(scratch, "source")
    dest = os.path.join(scratch, "dest")
    captures = os.path.join(scratch, "captures")

    write(os.path.join(source, "same.txt"), "same\n")
    write(os.path.join(source, "changed.txt"), "new\n")
    write(os.path.join(source, "added.txt"), "added\n")
    write(os.path.join(source, "sub", "nested_added.txt"), "nested\n")
    write(os.path.join(source, "sub", "data", "nested.txt"), "not the top-level data\n")
    write(os.path.join(source, "run.sh"), "#!/bin/sh\n", 0o755)
    write(os.path.join(source, "was_file", "inner.txt"), "inner\n")
    write(os.path.join(source, "was_dir"), "now a file\n")
    write(os.path.join(source, "data", "committed.txt"), "must not reach the captures\n")
    os.symlink("same.txt", os.path.join(source, "link"))
    os.symlink("added.txt", os.path.join(source, "relinked"))

    write(os.path.join(captures, "real.txt"), "capture\n")
    write(os.path.join(dest, "same.txt"), "same\n")
    write(os.path.join(dest, "changed.txt"), "old\n")
    write(os.path.join(dest, "removed.txt"), "removed\n")
    write(os.path.join(dest, "removed_dir", "x.txt"), "x\n")
    write(os.path.join(dest, "sub", "removed_nested.txt"), "removed\n")
    write(os.path.join(dest, "run.sh"), "#!/bin/sh\n", 0o644)
    write(os.path.join(dest, "was_file"), "a file\n")
    write(os.path.join(dest, "was_dir", "y.txt"), "y\n")
    os.symlink("same.txt", os.path.join(dest, "link"))
    os.symlink("same.txt", os.path.join(dest, "relinked"))
    os.symlink(captures, os.path.join(dest, "data"))
    age(dest)
    data_before = os.lstat(os.path.join(dest, "data"))

    code, output = run_sync("--keep", "data", source, dest)
    yield "sync: exits 0", code, 0, output
    yield "sync: a changed file is replaced", read(os.path.join(dest, "changed.txt")), "new\n", ""
    yield (
        "sync: a replaced file gets a new mtime",
        mtime_ns(os.path.join(dest, "changed.txt")) > OLD_NS,
        True,
        "",
    )
    yield (
        "sync: an unchanged file keeps its mtime",
        mtime_ns(os.path.join(dest, "same.txt")),
        OLD_NS,
        "",
    )
    yield (
        "sync: a mode change keeps the mtime",
        mtime_ns(os.path.join(dest, "run.sh")),
        OLD_NS,
        "",
    )
    yield (
        "sync: the mode follows the source",
        oct(stat.S_IMODE(os.lstat(os.path.join(dest, "run.sh")).st_mode)),
        oct(0o755),
        "",
    )
    for name in ("removed.txt", "removed_dir", os.path.join("sub", "removed_nested.txt")):
        gone = not os.path.lexists(os.path.join(dest, name))
        yield f"sync: removed {name} is deleted", gone, True, ""
    yield (
        "sync: an unchanged link keeps its mtime",
        mtime_ns(os.path.join(dest, "link")),
        OLD_NS,
        "",
    )
    data = os.path.join(dest, "data")
    yield "sync: the data link is still a link", os.path.islink(data), True, ""
    yield "sync: the data link's target is kept", os.readlink(data), captures, ""
    data_after = os.lstat(data)
    yield (
        "sync: the data link is untouched",
        (data_after.st_ino, data_after.st_mtime_ns),
        (data_before.st_ino, data_before.st_mtime_ns),
        "",
    )
    yield "sync: nothing is written through the data link", sorted(os.listdir(captures)), [
        "real.txt"
    ], ""
    want = listing(source, skip_top={"data"})
    got = listing(dest, skip_top={"data"})
    differ = sorted(set(want.items()) ^ set(got.items()))
    yield (
        "sync: the tree matches the source, kinds, modes, contents and links",
        len(differ),
        0,
        "\n".join(repr(entry) for entry in differ),
    )

    code, output = run_sync("--keep", "data", source, dest)
    tail = output.strip().splitlines()[-1] if output.strip() else ""
    yield (
        "sync: a second run changes nothing",
        tail.startswith("sync_tree: 0 copied,") and tail.endswith(", 0 deleted"),
        True,
        output,
    )
    code, output = run_sync("--keep", "sub/data", source, dest)
    yield "sync: --keep takes only a top-level name", code, 2, output


def tool_dir(scratch):
    """A PATH directory holding only the tools the verifier needs, so no rsync is reachable."""
    bin_dir = os.path.join(scratch, "bin")
    os.makedirs(bin_dir)
    missing = []
    for tool in TOOLS:
        found = shutil.which(tool)
        if found is None:
            missing.append(tool)
        else:
            os.symlink(found, os.path.join(bin_dir, tool))
    os.symlink(sys.executable, os.path.join(bin_dir, "python3"))
    return bin_dir, missing


def git(repo, *arguments):
    done = subprocess.run(
        [
            "git", "-C", repo, "-c", "user.name=Test", "-c", "user.email=test@example.com",
            "-c", "commit.gpgsign=false", "-c", f"core.hooksPath={repo}/.no-hooks", *arguments,
        ],
        capture_output=True, text=True, check=True,
    )  # fmt: skip
    return done.stdout.strip()


def verify(repo, bin_dir, commit_range):
    environment = dict(os.environ, PATH=bin_dir)
    environment.pop("AADP_JOBS", None)
    done = subprocess.run(
        [os.path.join(bin_dir, "bash"), os.path.join(repo, "scripts", "verify_commits.sh"),
         "--jobs", "1", commit_range],
        cwd=repo, env=environment, capture_output=True, text=True, check=False,
    )  # fmt: skip
    return done.returncode, done.stdout + done.stderr


def verify_cases(scratch):
    """The verifier syncs commit after commit with no rsync on PATH."""
    repo = os.path.join(scratch, "repo")
    bin_dir, missing = tool_dir(scratch)
    yield "verify_commits: the tools it needs are on PATH", missing, [], ""
    if missing:
        return
    yield "verify_commits: rsync is not on its PATH", shutil.which("rsync", path=bin_dir), None, ""

    os.makedirs(os.path.join(repo, "scripts"))
    for script in (VERIFY, SYNC):
        shutil.copy2(script, os.path.join(repo, "scripts"))
    git(repo, "init", "-q")
    write(os.path.join(repo, "CMakeLists.txt"), "cmake_minimum_required(VERSION 3.25)\n")
    write(os.path.join(repo, "same.txt"), "same\n")
    write(os.path.join(repo, "changed.txt"), "first\n")
    write(os.path.join(repo, "removed.txt"), "removed\n")
    git(repo, "add", "--", "CMakeLists.txt", "same.txt", "changed.txt", "removed.txt")
    git(repo, "commit", "-q", "-m", "first")
    first = git(repo, "rev-parse", "HEAD")
    write(os.path.join(repo, "changed.txt"), "second\n")
    write(os.path.join(repo, "added.txt"), "added\n")
    write(os.path.join(repo, "data", "committed.txt"), "must not reach the captures\n")
    git(repo, "rm", "-q", "--", "removed.txt")
    git(repo, "add", "--", "changed.txt", "added.txt", "data/committed.txt")
    git(repo, "commit", "-q", "-m", "second | with a bar")
    second = git(repo, "rev-parse", "HEAD")
    # The working tree's data/ stands for the real captures, which no commit holds.
    shutil.rmtree(os.path.join(repo, "data"))
    write(os.path.join(repo, "data", "real.txt"), "capture\n")

    # Exit 1: the commit fails to configure. 2 would mean the verifier could not start.
    code, output = verify(repo, bin_dir, first)
    yield "verify_commits: the first run exits 1", code, 1, output
    src = os.path.join(repo, "build", "commit-check", "src")
    data = os.path.join(src, "data")
    yield "verify_commits: the first run links data", os.path.islink(data), True, output
    if not os.path.islink(data):
        return
    age(src)
    data_before = os.lstat(data)

    code, output = verify(repo, bin_dir, f"{first}..{second}")
    yield "verify_commits: the second run exits 1", code, 1, output
    short = git(repo, "rev-parse", "--short", second)
    summary = os.path.join(repo, "build", "logs", "commits", "summary.txt")
    lines = read(summary).splitlines() if os.path.isfile(summary) else []
    yield (
        "verify_commits: the summary line",
        lines,
        [f"{short} | second / with a bar | configure=fail | build=- | tests=- | verdict=fail"],
        output,
    )

    def in_src(name):
        return os.path.join(src, name)

    yield "verify_commits: COMMIT names the commit", content(in_src("COMMIT")), second + "\n", ""
    yield "verify_commits: a changed file is replaced", content(in_src("changed.txt")), "second\n", ""
    yield (
        "verify_commits: an unchanged file keeps its mtime",
        mtime_ns(in_src("same.txt")),
        OLD_NS,
        "",
    )
    gone = not os.path.lexists(in_src("removed.txt"))
    yield "verify_commits: a removed file is deleted", gone, True, ""
    yield "verify_commits: an added file is there", content(in_src("added.txt")), "added\n", ""
    yield "verify_commits: the data link is still a link", os.path.islink(data), True, ""
    if os.path.islink(data):
        data_after = os.lstat(data)
        yield (
            "verify_commits: the data link is untouched",
            (data_after.st_ino, data_after.st_mtime_ns),
            (data_before.st_ino, data_before.st_mtime_ns),
            "",
        )
        target = os.readlink(data)
        yield "verify_commits: the data link's target", target, os.path.join(repo, "data"), ""
    yield (
        "verify_commits: nothing is written through the data link",
        sorted(os.listdir(os.path.join(repo, "data"))),
        ["real.txt"],
        "",
    )


def main():
    if len(sys.argv) != 2:
        print("usage: verify_commits_test.py <scratch-dir>", file=sys.stderr)
        return 2
    # Physical, as the verifier resolves its own root with pwd -P.
    scratch = os.path.realpath(sys.argv[1])
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(os.path.join(scratch, "sync"))
    results = list(sync_cases(os.path.join(scratch, "sync")))
    results += list(verify_cases(scratch))
    failures = 0
    for name, got, expected, output in results:
        verdict = "ok" if got == expected else "FAILED"
        print(f"{verdict:6} {name}: {got!r}, expected {expected!r}")
        if got != expected:
            failures += 1
            for line in output.splitlines():
                print("       " + line)
    print(f"verify_commits_test: {len(results) - failures} of {len(results)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
