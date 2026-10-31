#!/usr/bin/env python3
"""Tests of scripts/fault_injection.py, on a small throwaway CMake project.

    python3 -I faults/test_fault_injection.py

Needs cmake, ninja and a C++ compiler, and takes under a minute. Scratch files go to
build/fault-selftest/. Each test copies the runner into a project of its own and drives it as a
user would, so the exit code and the output are what is checked.
"""

import json
import os
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RUNNER = REPO / "scripts" / "fault_injection.py"
SCRATCH = REPO / "build" / "fault-selftest"

PRESETS = {
    "version": 6,
    "configurePresets": [
        {
            "name": "release",
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/build/release",
            "cacheVariables": {"CMAKE_BUILD_TYPE": "Release"},
        }
    ],
}
LISTS = """cmake_minimum_required(VERSION 3.25)
project(tiny CXX)
add_executable(tiny_check main.cpp)
set_target_properties(tiny_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
"""
MAIN = """#include <cstdio>
#include <cstring>
static const int kLimit = 3;
int Add(int a, int b) { return a + b; }
int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--fail") == 0) return 1;
  if (argc > 1 && std::strcmp(argv[1], "--chatty") == 0) std::puts("ADD IS WRONG");
  if (Add(2, 3) != 5) {
    std::puts("ADD IS WRONG");
    return 1;
  }
  if (kLimit != 3) {
    std::puts("LIMIT IS WRONG");
    return 2;
  }
  return 0;
}
"""


def fault(fault_id, edits, **overrides):
    entry = {
        "id": fault_id,
        "kind": "oracle",
        "covers": "the adder",
        "bug": "A deliberate bug for the runner's own tests.",
        "edits": edits,
        "preset": "release",
        "targets": ["tiny_check"],
        "command": ["{build}/bin/tiny_check"],
        "detect": "test",
        "reason": "ADD IS WRONG",
        "timeout_s": 20,
    }
    entry.update(overrides)
    return entry


def swap(find, replace, file="main.cpp"):
    return {"file": file, "find": find, "replace": replace}


class Project:
    def __init__(self, name, faults):
        self.root = SCRATCH / name
        if self.root.exists():
            shutil.rmtree(self.root)
        (self.root / "scripts").mkdir(parents=True)
        (self.root / "faults").mkdir()
        shutil.copy(RUNNER, self.root / "scripts" / "fault_injection.py")
        (self.root / "CMakePresets.json").write_text(json.dumps(PRESETS))
        (self.root / "CMakeLists.txt").write_text(LISTS)
        (self.root / "main.cpp").write_text(MAIN)
        self.write_manifest(faults)

    def write_manifest(self, faults):
        text = json.dumps({"schema": 1, "faults": faults}, indent=1)
        (self.root / "faults" / "manifest.json").write_text(text)

    def commit(self):
        identity = ["-c", "user.name=test", "-c", "user.email=test@example.invalid"]
        for command in (["init", "-q"], ["add", "-A"], ["commit", "-q", "-m", "x"]):
            subprocess.run(["git", "-C", str(self.root), *identity, *command], check=True)

    def run(self, *arguments):
        return subprocess.run(
            [sys.executable, "-I", "scripts/fault_injection.py", *arguments],
            cwd=self.root,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    def committed(self, faults=None):
        if faults is not None:
            self.write_manifest(faults)
        self.commit()
        return self

    def rows(self, output):
        rows = {}
        for line in output.splitlines():
            fields = [part.strip() for part in line.split(" | ")]
            if len(fields) == 6 and not line.startswith("columns:"):
                rows[fields[0]] = fields
        return rows


class Verdicts(unittest.TestCase):
    def verdict(self, name, *faults):
        project = Project(name, list(faults)).committed()
        done = project.run()
        return done, project.rows(done.stdout), project

    def test_a_real_bug_is_caught_and_the_tree_is_left_alone(self):
        done, rows, project = self.verdict("caught", fault("real", [swap("a + b", "a - b")]))
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertEqual(rows["real"][1:], ["the adder", "pass", "1", "ADD IS WRONG", "caught"])
        self.assertEqual((project.root / "main.cpp").read_text(), MAIN)
        self.assertEqual((project.root / "build/fault/copy/main.cpp").read_text(), MAIN)

    def test_a_fault_that_changes_no_behaviour_is_not_caught(self):
        done, rows, _ = self.verdict("no-change", fault("same", [swap("a + b", "b + a")]))
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(rows["same"][3:], ["0", "command passed", "not-caught"])

    def test_a_find_matching_nothing_is_not_applied(self):
        done, rows, _ = self.verdict("nothing", fault("gone", [swap("a * b", "a - b")]))
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(rows["gone"][5], "not-applied")
        self.assertIn("find matched 0 times", rows["gone"][4])

    def test_a_find_matching_twice_is_not_applied(self):
        done, rows, _ = self.verdict("twice", fault("two", [swap("ADD IS WRONG", "ADD")]))
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(rows["two"][5], "not-applied")
        self.assertIn("find matched 2 times", rows["two"][4])

    def test_overlapping_matches_count(self):
        done, rows, _ = self.verdict("overlap", fault("lap", [swap("  ", "\t")]))
        self.assertEqual(rows["lap"][5], "not-applied", done.stdout)
        self.assertNotIn("find matched 1 times", rows["lap"][4])

    def test_a_compile_error_in_test_mode_is_not_caught(self):
        done, rows, _ = self.verdict("compile", fault("broken", [swap("a + b", "a + nowhere")]))
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(rows["broken"][3], "build-failed")
        self.assertEqual(rows["broken"][5], "not-caught")
        self.assertIn("did not compile", rows["broken"][4])

    def test_a_failure_for_another_reason_is_not_caught(self):
        done, rows, _ = self.verdict(
            "other", fault("other", [swap("kLimit = 3", "kLimit = 4")], reason="ADD IS WRONG")
        )
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(rows["other"][3], "2")
        self.assertEqual(rows["other"][5], "not-caught")

    def test_a_timeout_is_not_caught_even_if_the_reason_printed(self):
        hang = 'std::puts("ADD IS WRONG"); std::fflush(stdout); for (volatile int i = 0;;) i = 0;\n'
        done, rows, _ = self.verdict(
            "timeout", fault("hang", [swap("  if (kLimit != 3) {", hang + "  if (kLimit != 3) {")],
                             timeout_s=2)
        )
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(rows["hang"][3:], ["timeout", "timed out after 2 s", "not-caught"])

    def test_build_mode_catches_a_named_compile_error(self):
        edit = {"file": "main.cpp", "append": '\nstatic_assert(1 == 2, "NAMED REASON");\n'}
        done, rows, _ = self.verdict(
            "build", fault("assert", [edit], detect="build", reason="NAMED REASON")
        )
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertEqual(rows["assert"][5], "caught")

    def test_build_mode_needs_the_build_to_fail_for_the_named_reason(self):
        done, rows, _ = self.verdict(
            "build-other",
            fault("wrong", [swap("a + b", "a + nowhere")], detect="build", reason="NAMED REASON"),
            fault("fine", [swap("a + b", "b + a")], detect="build", reason="NAMED REASON"),
        )
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(rows["wrong"][5], "not-caught")
        self.assertEqual(rows["fine"][3:], ["0", "build succeeded", "not-caught"])

    def test_a_failing_baseline_is_reported_as_such(self):
        done, rows, _ = self.verdict(
            "baseline",
            fault("bad", [swap("a + b", "a - b")], command=["{build}/bin/tiny_check", "--fail"]),
        )
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(rows["bad"][2], "fail")
        self.assertEqual(rows["bad"][5], "baseline-failed")

    def test_a_regex_matching_the_baseline_proves_nothing(self):
        done, rows, _ = self.verdict(
            "chatty",
            fault("loud", [swap("a + b", "a - b")], command=["{build}/bin/tiny_check", "--chatty"]),
        )
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(rows["loud"][5], "baseline-failed")
        self.assertIn("matches the passing baseline", rows["loud"][4])

    def test_several_edits_and_a_restore_between_faults(self):
        done, rows, project = self.verdict(
            "several",
            fault(
                "pair",
                [
                    swap("a + b", "a - b"),
                    {"file": "main.cpp", "append": "\n// A second edit.\n"},
                ],
            ),
            fault("half", [swap("a + b", "a - b"), swap("no such text", "x")]),
            # Only correct if neither mutation above outlived its fault in the objects.
            fault("after", [swap("kLimit = 3", "kLimit = 4")], reason="LIMIT IS WRONG"),
        )
        self.assertEqual(rows["pair"][5], "caught", done.stdout)
        self.assertEqual(rows["half"][5], "not-applied", done.stdout)
        self.assertEqual(rows["after"][2:], ["pass", "2", "LIMIT IS WRONG", "caught"])
        self.assertEqual((project.root / "build/fault/copy/main.cpp").read_text(), MAIN)


class Refusals(unittest.TestCase):
    def test_a_path_leaving_the_copy_is_refused_before_anything_runs(self):
        for name, path in (("dots", "../../outside.txt"), ("absolute", "/outside.txt")):
            project = Project(name, [fault("escape", [swap("x", "y", file=path)])]).committed()
            done = project.run()
            self.assertEqual(done.returncode, 2, done.stdout)
            self.assertIn("refusing path outside the copy", done.stdout)
            self.assertFalse((project.root / "build/fault/copy").exists())

    def test_a_symlink_leaving_the_copy_is_refused_and_its_target_kept(self):
        project = Project("symlink", [fault("link", [swap("KEEP", "GONE", file="link.txt")])])
        outside = SCRATCH / "symlink-target.txt"
        outside.write_text("KEEP\n")
        (project.root / "link.txt").symlink_to(outside)
        done = project.committed().run()
        rows = project.rows(done.stdout)
        self.assertEqual(rows["link"][5], "not-applied", done.stdout)
        self.assertIn("resolves outside the copy", rows["link"][4])
        self.assertEqual(outside.read_text(), "KEEP\n")

    def test_jobs_above_the_limit_are_refused(self):
        project = Project("jobs", [fault("real", [swap("a + b", "a - b")])]).committed()
        done = project.run("--jobs", "7")
        self.assertEqual(done.returncode, 2, done.stdout)
        self.assertIn("--jobs must be between 1 and 6", done.stdout)

    def test_unknown_fault_and_bad_manifest_could_not_run(self):
        project = Project("bad", [fault("real", [swap("a + b", "a - b")])]).committed()
        done = project.run("--only", "nope")
        self.assertEqual(done.returncode, 2, done.stdout)
        self.assertIn("no such fault", done.stdout)
        entry = fault("real", [swap("a + b", "a - b")])
        del entry["bug"]
        project = Project("schema", [entry]).committed()
        self.assertEqual(project.run().returncode, 2)

    def test_a_broken_preset_could_not_run(self):
        project = Project("preset", [fault("real", [swap("a + b", "a - b")], preset="nope")])
        done = project.committed().run()
        self.assertEqual(done.returncode, 2, done.stdout)


class Modes(unittest.TestCase):
    def test_source_mode_reads_an_export_with_no_git(self):
        project = Project("export-root", [])
        source = project.root / "src"
        source.mkdir()
        for name in ("CMakePresets.json", "CMakeLists.txt", "main.cpp", "faults"):
            shutil.move(project.root / name, source / name)
        (source / "COMMIT").write_text("abc1234\n")
        Project.write_manifest(
            type("Export", (), {"root": source}), [fault("real", [swap("a + b", "a - b")])]
        )
        done = project.run("--source", "src", "--output", "out/result.txt")
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertIn("commit: abc1234", done.stdout)
        self.assertTrue((project.root / "build/fault/copy/main.cpp").exists())
        self.assertEqual((source / "main.cpp").read_text(), MAIN)

    def test_committed_code_is_what_runs_not_the_working_tree(self):
        project = Project("committed", [fault("real", [swap("a + b", "a - b")])]).committed()
        (project.root / "main.cpp").write_text(MAIN.replace("a + b", "a + b + 0"))
        done = project.run()
        self.assertEqual(done.returncode, 0, done.stdout)

    def test_select_list_and_output(self):
        faults = [
            fault("one", [swap("a + b", "a - b")]),
            fault("two", [swap("kLimit = 3", "kLimit = 4")], kind="check",
                  reason="LIMIT IS WRONG"),
        ]
        project = Project("select", faults).committed()
        listed = project.run("--list", "--kind", "check")
        self.assertEqual(listed.returncode, 0, listed.stdout)
        self.assertEqual(listed.stdout.splitlines(),
                         ["two | check | test | the adder"])
        done = project.run("--only", "two", "--output", "build/result.txt")
        self.assertEqual(done.returncode, 0, done.stdout)
        text = (project.root / "build/result.txt").read_text()
        self.assertIn("claim: none\n", text)
        self.assertIn("compiler: ", text)
        self.assertIn("host_label: ", text)
        self.assertIn("utc_time: ", text)
        self.assertRegex(text, r"commit: [0-9a-f]{40}\n")
        self.assertIn("summary: caught=1 not-caught=0 not-applied=0 baseline-failed=0", text)
        self.assertNotIn(str(Path.home()), text)
        self.assertNotIn(os.environ.get("USER", "no user here"), text)

    def test_a_path_in_a_matched_reason_is_scrubbed(self):
        hook = swap("a + b", "a - b")
        marker = f'std::puts("{Path.home()}/secret/thing.cpp ADD IS WRONG");'
        edit = swap('std::puts("ADD IS WRONG");\n    return 1;', marker + "\n    return 1;")
        project = Project(
            "scrub", [fault("path", [hook, edit], reason=r"\S*thing\.cpp ADD IS WRONG")]
        ).committed()
        done = project.run("--output", "build/result.txt")
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertIn("<home>/secret/thing.cpp", (project.root / "build/result.txt").read_text())


if __name__ == "__main__":
    SCRATCH.mkdir(parents=True, exist_ok=True)
    unittest.main()
