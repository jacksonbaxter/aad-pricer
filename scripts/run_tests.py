#!/usr/bin/env python3
"""Runs ctest and the fuzz targets, for check.sh and CI, so both judge a run the same way.

    python3 -I scripts/run_tests.py ctest <preset> [--jobs <n>] [--allow-skipped <regex>]
                                    [--junit <file>] [--test-timeout <s>]
    python3 -I scripts/run_tests.py verdict <junit-file> [--allow-skipped <regex>]
    python3 -I scripts/run_tests.py fuzz --seconds <n> [--preset <preset>] [--only <target,...>]
                                    [--slice <s>] [--margin <s>] [--wrapper <program>]
                                    [--wrapper-wait <s>]

A test that ctest did not run, a disabled test, and a passing test whose output has a line
starting "SKIP " all count as skipped. A skipped test fails the run unless the regex names it,
and then it is printed and counted. The last line of a ctest or verdict run carries the counts.

A sanitizer report cannot hang a run. A sanitizer runs a symbolizer to name the frames of its
report, and the system's own can hang when the machine's developer authorization has lapsed. So
ctest and fuzz runs set each of ASAN_SYMBOLIZER_PATH, UBSAN_SYMBOLIZER_PATH and
TSAN_SYMBOLIZER_PATH that the caller left unset to the llvm-symbolizer beside the compiler named in
the build tree's CMakeCache.txt (build/<configure preset>, where every preset puts it), and print
which. If there is none there, they leave the environment alone and say so. And ctest runs a
sanitizer build (-fsanitize= in CMAKE_CXX_FLAGS) with a per-test timeout of 1500 s, CMake's own
default for a project that includes CTest: this one does not, so ctest's is 10,000,000 s. A test
with a TIMEOUT of its own keeps it, and --test-timeout sets another for any build (0: none).

Fuzzing. Targets are found with `ctest --show-only`, so a new target needs no edit here, and
--only picks some by name. Each target gets --seconds in total, as slices of at most --slice
seconds (default 300), each its own libFuzzer process: as many full slices as fit, then one with
the rest. Each has the arguments a run had before slicing, with -max_total_time set to its own
seconds: -dict when the target has one, -artifact_prefix, -print_final_stats=1, the corpus
directory, the tracked seeds (only read). So a run of at most one slice is one process, as before.
All slices of a target share build/fuzz-smoke/<target>/, emptied once when the target starts: each
starts from the corpus the slices before it wrote. libFuzzer's memory limit (2048 MB) is not
changed.

Why slices: the sanitizer runtime keeps bookkeeping for every thread a process ever made, and the
pricer starts fresh threads on every call, so one 30-minute process of a threaded target can reach
the memory limit with no fault in the code (docs/NOTES.md, "Stage 11"). The cost: the memory limit
now sees only growth that reaches it within one slice. Memory that stays reachable and grows too
slowly to reach 2048 MB in a slice is no longer caught by anything. Memory that is lost is: on
macOS, where ASan leaves leak detection off, fuzz runs turn it on (ASAN_OPTIONS detect_leaks=1,
unless the caller's ASAN_OPTIONS names detect_leaks), with LSAN_OPTIONS suppressing the one false
report it gives there, libFuzzer's own memory-watch thread (scripts/lsan_suppressions.txt, unless
the caller's LSAN_OPTIONS names suppressions). On Linux ASan has it on already. A leak is reported
when libFuzzer sees an input allocate more than it frees, or at the latest when the slice ends.

A slice that fails, by any exit but 0 (a crash, a sanitizer report, a leak, out of memory), fails
its target, which keeps the slice's log and artifact and runs no more slices: they would start from
the same corpus and find the same input again, and the first failure is the one to read. The other
targets still run.

Every slice has a hard limit of its seconds plus --margin (default 120 s), for libFuzzer's start,
its replay of the corpus before it counts time, and a long last input. libFuzzer's own limit on
one input (-timeout) is 1200 s, longer than the margin, so an input that runs past the margin is
stopped here, with its log and no artifact. On the limit the slice's process group, which the slice
leads, is killed, and the target is recorded as "timed out", never as a crash or a pass. After a
slice ends, anything left in its group is killed too, so nothing it started outlives it.

--wrapper <program> runs each slice as <program> <target> <args...>, for example through a lock
wrapper that waits while a measurement holds the machine, so that a measurement can take it between
two slices. The wrapper must print nothing before it starts the target, and must exec it or keep it
in its own process group. The slice's limit starts at the slice's first byte of output, which
libFuzzer writes as it starts, so the wrapper's wait is not counted. That wait has its own bound,
--wrapper-wait (default 21600 s, 6 hours): a slice that has printed nothing by then is killed and
recorded as "timed out waiting for the wrapper". Without a wrapper the limit starts with the slice.

Output: for a target run in more than one slice, a line for each slice with its result and the
peak resident memory libFuzzer reports (stat::peak_rss_mb, or the largest rss of its status lines
when it printed no final stats); for every target a line with its result and the largest peak.

Exit codes: 0 pass, 1 failure, 2 could not run.
"""

import argparse
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ElementTree

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
PASSED_OUTPUT_BYTES = 10_000_000
INNER_SKIP = re.compile(r"^SKIP\s", re.MULTILINE)
SYMBOLIZER_VARIABLES = ("ASAN_SYMBOLIZER_PATH", "UBSAN_SYMBOLIZER_PATH", "TSAN_SYMBOLIZER_PATH")
CACHE_ENTRY = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):[A-Z_]+=(.*)$", re.MULTILINE)
SANITIZER_TEST_TIMEOUT_S = 1500
SLICE_S = 300
MARGIN_S = 120
WRAPPER_WAIT_S = 6 * 3600
POLL_S = 0.25
LEAK_SUPPRESSIONS = os.path.join(REPO, "scripts", "lsan_suppressions.txt")
FINAL_PEAK = re.compile(r"^stat::peak_rss_mb:\s*(\d+)\s*$", re.MULTILINE)
STATUS_RSS = re.compile(r" rss: (\d+)Mb")


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


def build_dir(preset):
    """build/<configure preset>: every preset here inherits that binaryDir."""
    configure = preset
    try:
        with open(os.path.join(REPO, "CMakePresets.json")) as file:
            presets = json.load(file)
        for test in presets.get("testPresets", []):
            if test.get("name") == preset and test.get("configurePreset"):
                configure = test["configurePreset"]
    except (OSError, ValueError, AttributeError):
        pass
    return os.path.join(REPO, "build", configure)


def read_cache(folder):
    """The CMake cache's entries, or None when the folder has no cache."""
    try:
        with open(os.path.join(folder, "CMakeCache.txt"), errors="replace") as file:
            return dict(CACHE_ENTRY.findall(file.read()))
    except OSError:
        return None


def symbolizer_environment(environment, cache, where):
    """Returns the environment for the tests and the line that says which symbolizer they get."""
    environment = dict(environment)
    unset = [name for name in SYMBOLIZER_VARIABLES if not environment.get(name)]
    if not unset:
        return environment, "symbolizer: as the caller set it, " + ", ".join(
            f"{name}={environment[name]}" for name in SYMBOLIZER_VARIABLES
        )
    if cache is None:
        return environment, f"symbolizer: no CMakeCache.txt in {where}, environment left alone"
    compiler = cache.get("CMAKE_CXX_COMPILER") or cache.get("CMAKE_C_COMPILER")
    if not compiler:
        return environment, f"symbolizer: no compiler in {where}'s cache, environment left alone"
    if not os.path.isabs(compiler):
        compiler = shutil.which(compiler) or compiler
    folders = []
    for folder in (os.path.dirname(compiler), os.path.dirname(os.path.realpath(compiler))):
        if folder and folder not in folders:
            folders.append(folder)
    for folder in folders:
        path = os.path.join(folder, "llvm-symbolizer")
        if os.path.isfile(path) and os.access(path, os.X_OK):
            for name in unset:
                environment[name] = path
            kept = [name for name in SYMBOLIZER_VARIABLES if name not in unset]
            note = f", {' and '.join(kept)} as the caller set it" if kept else ""
            return environment, f"symbolizer: {path}, beside the compiler of {where}{note}"
    return environment, (
        f"symbolizer: no llvm-symbolizer beside {compiler}, environment left alone, so the "
        "sanitizers choose their own"
    )


def test_timeout(arguments, cache):
    """Seconds for ctest --timeout, or None for ctest's own default, and the line saying so."""
    if arguments.test_timeout is not None:
        if arguments.test_timeout == 0:
            return None, "per-test timeout: ctest's default, as --test-timeout 0 asks"
        return arguments.test_timeout, f"per-test timeout: {arguments.test_timeout} s, as asked"
    flags = (cache or {}).get("CMAKE_CXX_FLAGS", "")
    if "-fsanitize=" in flags:
        return SANITIZER_TEST_TIMEOUT_S, (
            f"per-test timeout: {SANITIZER_TEST_TIMEOUT_S} s for a test with no TIMEOUT of its "
            "own, in a sanitizer build"
        )
    return None, "per-test timeout: ctest's default, not a sanitizer build"


def run_ctest(arguments, allow):
    if arguments.jobs < 1:
        raise CannotRun("--jobs must be positive")
    if arguments.test_timeout is not None and arguments.test_timeout < 0:
        raise CannotRun("--test-timeout must not be negative")
    junit = os.path.abspath(
        arguments.junit or os.path.join(REPO, "build", f"ctest-{arguments.preset}-junit.xml")
    )
    os.makedirs(os.path.dirname(junit), exist_ok=True)
    if os.path.exists(junit):
        os.remove(junit)
    folder = build_dir(arguments.preset)
    cache = read_cache(folder)
    where = os.path.relpath(folder, REPO)
    environment, line = symbolizer_environment(os.environ, cache, where)
    print(f"run_tests: {line}")
    timeout, line = test_timeout(arguments, cache)
    print(f"run_tests: {line}")
    sys.stdout.flush()
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
    if timeout is not None:
        command += ["--timeout", str(timeout)]
    try:
        status = subprocess.run(command, cwd=REPO, env=environment, check=False).returncode
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


def select_targets(targets, only):
    if not only:
        return targets
    wanted = [name for part in only for name in part.split(",") if name]
    unknown = [name for name in wanted if name not in {target[0] for target in targets}]
    if unknown:
        raise CannotRun(f"no such fuzz target: {', '.join(unknown)}")
    return [target for target in targets if target[0] in wanted]


def dictionary_for(name):
    # fuzz_taifex_options shares fuzz_taifex.dict with fuzz_taifex_futures.
    stem = name.removeprefix("aadp_")
    for candidate in (stem, stem.rsplit("_", 1)[0]):
        path = os.path.join(REPO, "fuzz", f"{candidate}.dict")
        if os.path.exists(path):
            return path
    return None


def slice_plan(seconds, slice_seconds):
    """The seconds of each slice: as many full slices as fit, then one with the rest."""
    full, rest = divmod(seconds, slice_seconds)
    return [slice_seconds] * full + ([rest] if rest else [])


def peak_rss(text):
    """libFuzzer's peak resident memory in MB and where it came from, or (None, None)."""
    final = FINAL_PEAK.findall(text)
    if final:
        return int(final[-1]), "final stats"
    status = STATUS_RSS.findall(text)
    if status:
        return max(int(value) for value in status), "status lines"
    return None, None


def peak_text(peak, source):
    if peak is None:
        return "peak_rss_mb=unknown"
    return f"peak_rss_mb={peak}" + ("" if source == "final stats" else f" (from {source})")


def leak_environment(environment, system):
    """Turns leak detection on where ASan leaves it off (macOS). Returns it and a line, or None."""
    if system != "Darwin":
        return environment, None
    environment = dict(environment)
    asan = environment.get("ASAN_OPTIONS", "")
    if "detect_leaks" in asan:
        return environment, "leak detection: as the caller's ASAN_OPTIONS sets it"
    environment["ASAN_OPTIONS"] = f"{asan}:detect_leaks=1" if asan else "detect_leaks=1"
    lsan = environment.get("LSAN_OPTIONS", "")
    note = "the caller's LSAN_OPTIONS suppressions"
    if "suppressions=" not in lsan:
        own = f"suppressions={LEAK_SUPPRESSIONS}"
        environment["LSAN_OPTIONS"] = f"{lsan}:{own}" if lsan else own
        note = os.path.relpath(LEAK_SUPPRESSIONS, REPO)
    return environment, f"leak detection: on (ASAN_OPTIONS detect_leaks=1), suppressions {note}"


def exited(pid):
    """Without reaping it, so that the group keeps the child's id until it is killed."""
    return os.waitid(os.P_PID, pid, os.WEXITED | os.WNOHANG | os.WNOWAIT) is not None


def kill_group(pid):
    try:
        os.killpg(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass  # Nothing is left in it.


def run_slice(command, log, limit, wrapper_wait, environment):
    """Returns (exit code, None), or (None, why) for a slice that was killed. The limit counts from
    the start, or with a wrapper from the first byte of output, which wrapper_wait bounds."""
    with open(log, "wb") as sink:
        try:
            child = subprocess.Popen(
                command,
                cwd=REPO,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=sink,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        except OSError as error:
            raise CannotRun(f"cannot run {command[0]}: {error}") from error
        launched = time.monotonic()
        started = None if wrapper_wait is not None else launched
        why = None
        try:
            while not exited(child.pid):
                now = time.monotonic()
                if started is None and os.fstat(sink.fileno()).st_size > 0:
                    started = now
                if started is None and now - launched > wrapper_wait:
                    why = f"timed out waiting for the wrapper, no output in {wrapper_wait} s, killed"
                    break
                if started is not None and now - started > limit:
                    why = f"timed out after {limit} s, killed"
                    break
                time.sleep(POLL_S)
        finally:
            kill_group(child.pid)
            code = child.wait()
    return (None, why) if why else (code, None)


def fuzz_command(binary, name, seconds, work, corpus, seeds):
    command = [
        binary,
        f"-max_total_time={seconds}",
        f"-artifact_prefix={work}{os.sep}",
        "-print_final_stats=1",
        corpus,
        *seeds,
    ]
    dictionary = dictionary_for(name)
    if dictionary is not None:
        command.insert(1, f"-dict={dictionary}")
    return command


def show_tail(log):
    with open(log, "rb") as file:
        tail = file.read().decode("utf-8", "replace").splitlines()[-30:]
    print("\n".join(tail))


def run_target(target, plan, arguments, environment):
    """Runs one target's slices in order and returns True if every one passed."""
    name, binary, recorded = target
    seeds = [word for word in recorded if not word.startswith("-")]
    work = os.path.join(REPO, "build", "fuzz-smoke", name)
    shutil.rmtree(work, ignore_errors=True)
    corpus = os.path.join(work, "corpus")
    os.makedirs(corpus)
    sliced = len(plan) > 1
    wait = arguments.wrapper_wait if arguments.wrapper else None
    largest = (None, None)
    failure = None
    for number, seconds in enumerate(plan, 1):
        command = fuzz_command(binary, name, seconds, work, corpus, seeds)
        if arguments.wrapper:
            command.insert(0, arguments.wrapper)
        log = os.path.join(work, f"slice-{number}.log" if sliced else "output.log")
        code, why = run_slice(command, log, seconds + arguments.margin, wait, environment)
        with open(log, "rb") as file:
            peak, source = peak_rss(file.read().decode("utf-8", "replace"))
        if peak is not None and (largest[0] is None or peak > largest[0]):
            largest = (peak, number)
        status = why or f"exit {code}"
        where = os.path.relpath(log, REPO)
        if sliced:
            print(
                f"run_tests: fuzz {name}: slice {number} of {len(plan)}, {seconds} s: {status}, "
                f"{peak_text(peak, source)}, output in {where}"
            )
        else:
            print(f"run_tests: fuzz {name}: {status}, output in {where}")
        if why or code != 0:
            show_tail(log)
            failure = f"failed in slice {number} of {len(plan)}: {status}"
            if number < len(plan):
                failure += f"; slices {number + 1} to {len(plan)} not run"
            sys.stdout.flush()
            break
        sys.stdout.flush()
    result = failure or "passed"
    count = f"{len(plan)} slice{'s' if sliced else ''}"
    peak = "largest peak_rss_mb=unknown"
    if largest[0] is not None:
        peak = f"largest peak_rss_mb={largest[0]} (slice {largest[1]})"
    print(f"run_tests: fuzz {name}: {result}, {count} of {sum(plan)} s, {peak}")
    sys.stdout.flush()
    return failure is None


def run_fuzz(arguments):
    for option in ("seconds", "slice"):
        if getattr(arguments, option) < 1:
            raise CannotRun(f"--{option} must be positive")
    if arguments.margin < 1 or arguments.wrapper_wait < 1:
        raise CannotRun("--margin and --wrapper-wait must be positive")
    if arguments.wrapper and shutil.which(arguments.wrapper) is None:
        raise CannotRun(f"no such program for --wrapper: {arguments.wrapper}")
    targets = fuzz_targets(arguments.preset)
    require_every_source_has_a_target(targets)
    chosen = select_targets(targets, arguments.only)
    plan = slice_plan(arguments.seconds, arguments.slice)
    print(f"run_tests: {len(targets)} fuzz targets, {arguments.seconds} s each")
    if len(chosen) < len(targets):
        print(f"run_tests: --only: {len(chosen)} of them, {', '.join(t[0] for t in chosen)}")
    if len(plan) > 1:
        print(
            f"run_tests: {len(plan)} slices a target, {', '.join(map(str, plan))} s, each limited "
            f"to its seconds plus {arguments.margin} s"
        )
    folder = build_dir(arguments.preset)
    environment, line = symbolizer_environment(
        os.environ, read_cache(folder), os.path.relpath(folder, REPO)
    )
    print(f"run_tests: {line}")
    environment, line = leak_environment(environment, platform.system())
    if line:
        print(f"run_tests: {line}")
    if arguments.wrapper:
        print(
            f"run_tests: each slice through {arguments.wrapper}, its limit counted from its "
            f"first output, the wait before it at most {arguments.wrapper_wait} s"
        )
    sys.stdout.flush()
    failed = sum(not run_target(target, plan, arguments, environment) for target in chosen)
    print(f"run_tests: fuzz total={len(chosen)} failed={failed}")
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description="Runs tests and judges skips.")
    commands = parser.add_subparsers(dest="command", required=True)

    ctest = commands.add_parser("ctest")
    ctest.add_argument("preset")
    ctest.add_argument("--jobs", type=int, default=1)
    ctest.add_argument("--allow-skipped")
    ctest.add_argument("--junit")
    ctest.add_argument("--test-timeout", type=int)

    verdict = commands.add_parser("verdict")
    verdict.add_argument("junit")
    verdict.add_argument("--allow-skipped")

    fuzz = commands.add_parser("fuzz")
    fuzz.add_argument("--seconds", type=int, required=True)
    fuzz.add_argument("--preset", default="fuzz")
    fuzz.add_argument("--only", action="append", default=[], help="targets, comma-separated")
    fuzz.add_argument("--slice", type=int, default=SLICE_S)
    fuzz.add_argument("--margin", type=int, default=MARGIN_S)
    fuzz.add_argument("--wrapper")
    fuzz.add_argument("--wrapper-wait", type=int, default=WRAPPER_WAIT_S)

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
