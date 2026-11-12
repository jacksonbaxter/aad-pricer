#!/usr/bin/env python3
"""The repo scan (scripts/scan_repo.py) on agent notes files, in memory and in a scratch repo.

    python3 -I tests/data/scan_repo_test.py <scratch-dir>

The scratch directory is emptied and holds a git repo of its own. Exit 0 when every case gives
its expected verdict. Standard library only.
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCAN = os.path.join(HERE, "..", "..", "scripts", "scan_repo.py")
# Importing must not write scripts/__pycache__ into the working tree.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(SCAN))

import scan_repo  # noqa: E402

# Split, so that this file does not match the scan's own markers.
NOTES = "CLAUDE" + ".md"
SETTINGS = "." + "claude/settings.json"

NOTE_PATHS = [
    NOTES,
    "CLAUDE" + ".local.md",
    "claude" + ".md",
    "docs/" + NOTES,
    "AGENTS.md",
    "GEMINI.md",
    ".cursorrules",
    ".windsurfrules",
    ".github/copilot-instructions.md",
    SETTINGS,
    ".cursor/rules/style.mdc",
]
PLAIN_PATHS = ["README.md", "docs/NOTES.md", "docs/TASKS.md", "src/claude_free.cpp"]

GITIGNORE = b"/." + b"claude/settings.local.json\n/" + NOTES.encode() + b"\n"


def note_findings(path=None, content=None):
    scan = scan_repo.Scan(None, None, None)
    if path is not None and content is None:
        scan.scan_path(path)
    else:
        scan.scan_notes(path, content)
    return [kind for _, _, kind in scan.findings].count("private-note")


def cases():
    for path in NOTE_PATHS:
        yield f"tracked {path} is a finding", note_findings(path), 1
    for path in PLAIN_PATHS:
        yield f"tracked {path} is not a finding", note_findings(path), 0
    yield "the two .gitignore lines are excused", note_findings(".gitignore", GITIGNORE), 0
    yield (
        "the same lines elsewhere are findings",
        note_findings("README.md", GITIGNORE),
        2,
    )
    yield (
        "another .gitignore line is a finding",
        note_findings(".gitignore", b"/" + NOTES.encode() + b".bak\n"),
        1,
    )


def git(scratch, *arguments):
    subprocess.run(
        ["git", "-C", scratch, *arguments], check=True, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )  # fmt: skip


def scan(scratch):
    done = subprocess.run(
        [sys.executable, "-I", SCAN, "--root", scratch], capture_output=True, text=True
    )
    return done.returncode, done.stdout


def repo_cases(scratch):
    """A root notes file on disk but untracked and ignored passes; tracked, it fails."""
    shutil.rmtree(scratch, ignore_errors=True)
    os.makedirs(scratch)
    for name, content in (
        ("README.md", b"A synthetic repo.\n"),
        (".gitignore", GITIGNORE),
        (NOTES, b"Instructions for a session.\n"),
    ):
        with open(os.path.join(scratch, name), "wb") as file:
            file.write(content)
    git(scratch, "init", "-q")
    git(scratch, "add", "--", "README.md", ".gitignore")
    code, output = scan(scratch)
    yield "untracked root notes file passes the scan", code, 0, output
    ignored = subprocess.run(
        ["git", "-C", scratch, "check-ignore", "-q", "--", NOTES], check=False
    ).returncode
    yield "the root notes file is ignored", ignored, 0, ""
    git(scratch, "add", "-f", "--", NOTES)
    code, output = scan(scratch)
    yield "tracked root notes file fails the scan", code, 1, output
    named = f"{NOTES}:0: private-note" in output.splitlines()
    yield "the finding names the tracked path", int(named), 1, output


def main():
    if len(sys.argv) != 2:
        print("usage: scan_repo_test.py <scratch-dir>", file=sys.stderr)
        return 2
    scratch = os.path.abspath(sys.argv[1])
    results = [(name, got, expected, "") for name, got, expected in cases()]
    results += list(repo_cases(scratch))
    failures = 0
    for name, got, expected, output in results:
        verdict = "ok" if got == expected else "FAILED"
        print(f"{verdict:6} {name}: {got}, expected {expected}")
        if got != expected:
            failures += 1
            for line in output.splitlines():
                print("       " + line)
    print(f"scan_repo_test: {len(results) - failures} of {len(results)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
