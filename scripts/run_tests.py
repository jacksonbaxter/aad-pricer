#!/usr/bin/env python3
"""Runs ctest and the fuzz smoke, for check.sh and CI, so both judge a run the same way.

    python3 -I scripts/run_tests.py ctest <preset> [--jobs <n>] [--allow-skipped <regex>]
                                    [--junit <file>]
    python3 -I scripts/run_tests.py verdict <junit-file> [--allow-skipped <regex>]
    python3 -I scripts/run_tests.py fuzz --seconds <n> [--preset <preset>]

A test that ctest did not run, a disabled test, and a passing test whose output has a line
starting "SKIP " all count as skipped. A skipped test fails the run unless the regex names it,
and then it is printed and counted. The last line of a ctest or verdict run carries the counts.

Fuzz targets are found with `ctest --show-only`, so a new target needs no edit here. Each runs
from a fresh directory under build/fuzz-smoke/. The tracked seeds are only read.

Exit codes: 0 pass, 1 failure, 2 could not run.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ElementTree

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
PASSED_OUTPUT_BYTES = 10_000_000
INNER_SKIP = re.compile(r"^SKIP\s", re.MULTILINE)


class CannotRun(Exception):
    pass


def read_junit(path):
    try:
        cases = ElementTree.parse(path).getroot().findall("testcase")
    except (OSError, ElementTree.ParseError) as error:
        raise CannotRun(f"cannot read {path}: {error}") from error
    if not cases:
        raise CannotRun(f"no test cases in {path}")
    return cases


def judge(junit, allow, ctest_status, label):
    passed = failed = 0
    skipped = []
    failures = []
    for case in read_junit(junit):
        name = case.get("name", "?")
        status = case.get("status")
        output = case.findtext("system-out") or ""
        if status == "fail":
            failed += 1
            failures.append(name)
        elif status in ("notrun", "disabled"):
            skipped.append(name)
        elif INNER_SKIP.search(output):
            skipped.append(name)
        else:
            passed += 1

    allowed = [name for name in skipped if allow is not None and allow.search(name)]
    refused = [name for name in skipped if name not in allowed]
    for name in failures:
        print(f"run_tests: failed: {name}")
    for name in allowed:
        print(f"run_tests: skipped, allowed by --allow-skipped: {name}")
    for name in refused:
        print(f"run_tests: skipped, not allowed: {name}")
    print(
        f"run_tests: {label} total={passed + failed + len(skipped)} passed={passed} "
        f"failed={failed} skipped={len(skipped)} allowed={len(allowed)}"
    )
    # ctest also fails for a test that could not start, which the file lists as not run.
    return 1 if ctest_status != 0 or failed or refused else 0


def run_ctest(arguments, allow):
    if arguments.jobs < 1:
        raise CannotRun("--jobs must be positive")
    junit = os.path.abspath(
        arguments.junit or os.path.join(REPO, "build", f"ctest-{arguments.preset}-junit.xml")
    )
    os.makedirs(os.path.dirname(junit), exist_ok=True)
    if os.path.exists(junit):
        os.remove(junit)
    command = [
        "ctest",
        "--preset",
        arguments.preset,
        "--parallel",
        str(arguments.jobs),
        "--output-junit",
        junit,
        "--test-output-size-passed",
        str(PASSED_OUTPUT_BYTES),
    ]
    try:
        status = subprocess.run(command, cwd=REPO, check=False).returncode
    except OSError as error:
        raise CannotRun(f"cannot run ctest: {error}") from error
    sys.stdout.flush()
    return judge(junit, allow, status, arguments.preset)


def fuzz_targets(preset):
    try:
        listing = subprocess.run(
            ["ctest", "--preset", preset, "--show-only=json-v1"],
            cwd=REPO,
            capture_output=True,
            check=False,
            text=True,
        )
    except OSError as error:
        raise CannotRun(f"cannot run ctest: {error}") from error
    if listing.returncode != 0:
        raise CannotRun(f"ctest --show-only failed: {listing.stderr.strip()}")
    targets = []
    for test in json.loads(listing.stdout).get("tests", []):
        labels = []
        for prop in test.get("properties", []):
            if prop.get("name") == "LABELS":
                labels = prop.get("value", [])
        if "fuzz" in labels:
            command = test.get("command")
            if not command:  # ctest lists a target it cannot find with no command.
                raise CannotRun(f"fuzz target {test['name']} is not built")
            targets.append((test["name"], command[0], command[1:]))
    if not targets:
        raise CannotRun(f"preset {preset} lists no fuzz targets")
    return sorted(targets)


def require_every_source_has_a_target(targets):
    names = {name for name, _, _ in targets}
    for entry in sorted(os.listdir(os.path.join(REPO, "fuzz"))):
        stem, extension = os.path.splitext(entry)
        if extension == ".cpp" and stem not in names and f"aadp_{stem}" not in names:
            raise CannotRun(f"fuzz/{entry} has no fuzz target in the build")


def dictionary_for(name):
    # fuzz_taifex_options shares fuzz_taifex.dict with fuzz_taifex_futures.
    stem = name.removeprefix("aadp_")
    for candidate in (stem, stem.rsplit("_", 1)[0]):
        path = os.path.join(REPO, "fuzz", f"{candidate}.dict")
        if os.path.exists(path):
            return path
    return None


def run_fuzz(arguments):
    if arguments.seconds < 1:
        raise CannotRun("--seconds must be positive")
    targets = fuzz_targets(arguments.preset)
    require_every_source_has_a_target(targets)
    print(f"run_tests: {len(targets)} fuzz targets, {arguments.seconds} s each")
    sys.stdout.flush()
    failed = 0
    for name, binary, recorded in targets:
        seeds = [word for word in recorded if not word.startswith("-")]
        work = os.path.join(REPO, "build", "fuzz-smoke", name)
        shutil.rmtree(work, ignore_errors=True)
        corpus = os.path.join(work, "corpus")
        os.makedirs(corpus)
        command = [
            binary,
            f"-max_total_time={arguments.seconds}",
            f"-artifact_prefix={work}{os.sep}",
            "-print_final_stats=1",
            corpus,
            *seeds,
        ]
        dictionary = dictionary_for(name)
        if dictionary is not None:
            command.insert(1, f"-dict={dictionary}")
        log = os.path.join(work, "output.log")
        with open(log, "wb") as file:
            try:
                status = subprocess.run(
                    command, cwd=REPO, stdout=file, stderr=subprocess.STDOUT, check=False
                ).returncode
            except OSError as error:
                raise CannotRun(f"cannot run {name}: {error}") from error
        print(f"run_tests: fuzz {name}: exit {status}, output in {os.path.relpath(log, REPO)}")
        if status != 0:
            failed += 1
            with open(log, "rb") as file:
                tail = file.read().decode("utf-8", "replace").splitlines()[-30:]
            print("\n".join(tail))
        sys.stdout.flush()
    print(f"run_tests: fuzz total={len(targets)} failed={failed}")
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description="Runs tests and judges skips.")
    commands = parser.add_subparsers(dest="command", required=True)

    ctest = commands.add_parser("ctest")
    ctest.add_argument("preset")
    ctest.add_argument("--jobs", type=int, default=1)
    ctest.add_argument("--allow-skipped")
    ctest.add_argument("--junit")

    verdict = commands.add_parser("verdict")
    verdict.add_argument("junit")
    verdict.add_argument("--allow-skipped")

    fuzz = commands.add_parser("fuzz")
    fuzz.add_argument("--seconds", type=int, required=True)
    fuzz.add_argument("--preset", default="fuzz")

    arguments = parser.parse_args()
    try:
        allow = None
        if getattr(arguments, "allow_skipped", None):
            allow = re.compile(arguments.allow_skipped)
        if arguments.command == "ctest":
            return run_ctest(arguments, allow)
        if arguments.command == "verdict":
            return judge(arguments.junit, allow, 0, "tests")
        return run_fuzz(arguments)
    except (CannotRun, re.error) as error:
        print(f"run_tests: could not run: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
