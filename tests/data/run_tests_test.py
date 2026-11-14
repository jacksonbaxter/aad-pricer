#!/usr/bin/env python3
"""Tests of scripts/run_tests.py: fuzz slices, their corpus, failures, timeouts and wrapper, the
peak-memory lines, --only, the symbolizer guard and ctest's per-test timeout.

    python3 -I tests/data/run_tests_test.py <scratch-dir> [unittest arguments, such as -k <name>]

Each test copies the runner into a small CMake project of its own under the scratch directory. The
project has no languages, so no compiler runs; its fuzz targets and tests are Python scripts that
record how they were run, and a fake toolchain folder stands for the compiler. The runner is driven
as check.sh and CI drive it, so its exit code and output are what is checked. Needs cmake, ctest
and ninja. Takes about a minute.
"""

import importlib.util
import json
import os
import shutil
import signal
import subprocess
import sys
import time
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
RUNNER = REPO / "scripts" / "run_tests.py"
SCRATCH = None
TARGETS = ("aadp_fuzz_alpha", "aadp_fuzz_beta", "aadp_fuzz_gamma")
CLEARED = (
    "ASAN_SYMBOLIZER_PATH", "UBSAN_SYMBOLIZER_PATH", "TSAN_SYMBOLIZER_PATH", "ASAN_OPTIONS",
    "LSAN_OPTIONS",
)  # fmt: skip

# A fake libFuzzer target. Each run is a slice: it counts its slices, records its arguments, the
# corpus it found and its environment, adds one unit to the corpus, and does what behaviour.json
# says for that slice: exit with a code, hang (with a child of its own), or print memory figures.
TARGET = """#!{python} -I
import json, os, subprocess, sys, time
NAME = {name!r}
state = os.environ["FAKE_FUZZ_STATE"]
args = sys.argv[1:]
positional = [word for word in args if not word.startswith("-")]
corpus = positional[0] if positional else None
counter = os.path.join(state, NAME + ".count")
number = int(open(counter).read()) + 1 if os.path.exists(counter) else 1
open(counter, "w").write(str(number))
with open(os.path.join(state, "behaviour.json")) as file:
    behaviour = json.load(file).get(NAME, {{}}).get(str(number), {{}})
found = sorted(os.listdir(corpus)) if corpus and os.path.isdir(corpus) else None
names = ("ASAN_SYMBOLIZER_PATH", "UBSAN_SYMBOLIZER_PATH", "TSAN_SYMBOLIZER_PATH", "ASAN_OPTIONS")
record = {{"name": NAME, "slice": number, "argv": args, "corpus": corpus, "found": found,
          "env": {{key: os.environ.get(key) for key in names}}, "pgid": os.getpgid(0)}}
with open(os.path.join(state, "calls.jsonl"), "a") as file:
    file.write(json.dumps(record) + "\\n")
print("INFO: fake target", NAME, "slice", number, flush=True)
if corpus and os.path.isdir(corpus):
    open(os.path.join(corpus, NAME + "-unit-" + str(number)), "w").write("x")
if behaviour.get("hang"):
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"])
    open(os.path.join(state, NAME + ".child"), "w").write(str(child.pid))
    time.sleep(120)
for rss in behaviour.get("status_rss", [20]):
    print("#2 NEW cov: 1 ft: 1 corp: 1/1b exec/s: 0 rss: " + str(rss) + "Mb")
if behaviour.get("final", True):
    print("stat::number_of_executed_units: 10")
    print("stat::peak_rss_mb:              " + str(behaviour.get("peak", 30)))
sys.exit(behaviour.get("exit", 0))
"""

# A lock wrapper: records each call, waits silently, then becomes the target.
WRAPPER = """#!{python} -I
import json, os, sys, time
with open(os.path.join(os.environ["FAKE_FUZZ_STATE"], "wrapper.jsonl"), "a") as file:
    file.write(json.dumps(sys.argv[1:]) + "\\n")
time.sleep(float(os.environ.get("FAKE_WRAPPER_SLEEP", "0")))
os.execv(sys.argv[1], sys.argv[1:])
"""

# A ctest test that records the symbolizer variables it gets.
ENV_PROBE = """#!{python} -I
import json, os, sys
names = ("ASAN_SYMBOLIZER_PATH", "UBSAN_SYMBOLIZER_PATH", "TSAN_SYMBOLIZER_PATH")
with open(os.path.join(os.environ["FAKE_FUZZ_STATE"], "probe.json"), "w") as file:
    json.dump({{key: os.environ.get(key) for key in names}}, file)
"""

# A ctest test that hangs, with a child that would outlive it.
HANG = """#!{python} -I
import os, subprocess, sys, time
child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"])
open(os.path.join(os.environ["FAKE_FUZZ_STATE"], "hang.child"), "w").write(str(child.pid))
time.sleep(120)
"""

LISTS = """cmake_minimum_required(VERSION 3.25)
project(fake NONE)
enable_testing()
foreach(name IN ITEMS {targets})
  add_test(NAME ${{name}} COMMAND "${{PROJECT_SOURCE_DIR}}/targets/${{name}}" -runs=0
           "${{PROJECT_SOURCE_DIR}}/seeds/${{name}}")
  set_tests_properties(${{name}} PROPERTIES LABELS "fuzz")
endforeach()
add_test(NAME env_probe COMMAND "${{PROJECT_SOURCE_DIR}}/targets/env_probe")
if(WITH_HANG)
  add_test(NAME hang COMMAND "${{PROJECT_SOURCE_DIR}}/targets/hang")
endif()
"""


def configure_preset(name, toolchain, flags, **extra):
    variables = {"CMAKE_CXX_COMPILER": f"${{sourceDir}}/{toolchain}/bin/clang++"}
    variables["CMAKE_CXX_FLAGS"] = flags
    variables.update(extra)
    return {
        "name": name,
        "generator": "Ninja",
        "binaryDir": "${sourceDir}/build/${presetName}",
        "cacheVariables": variables,
    }


PRESETS = {
    "version": 6,
    "configurePresets": [
        configure_preset("fuzz", "toolchain", "-fsanitize=fuzzer-no-link,address"),
        configure_preset("nosym", "toolchain-nosym", "-fsanitize=address"),
        configure_preset("plain", "toolchain", "-O2"),
        configure_preset("hangs", "toolchain", "-fsanitize=thread", WITH_HANG="ON"),
    ],
    "testPresets": [
        {"name": name, "configurePreset": name} for name in ("fuzz", "nosym", "plain", "hangs")
    ],
}


def executable(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(0o755)


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def gone_within(pid, seconds):
    """A killed process can be a zombie for a moment until its new parent reaps it."""
    deadline = time.monotonic() + seconds
    while alive(pid) and time.monotonic() < deadline:
        time.sleep(0.1)
    if alive(pid):
        os.kill(pid, signal.SIGKILL)  # Do not leave it behind.
        return False
    return True


class Project:
    """A fake project with the runner, three fuzz targets, an env probe, a hang and four presets,
    configured once per test class."""

    def __init__(self, name):
        self.root = SCRATCH / name
        if self.root.exists():
            shutil.rmtree(self.root)
        (self.root / "scripts").mkdir(parents=True)
        shutil.copy(RUNNER, self.root / "scripts" / "run_tests.py")
        (self.root / "CMakePresets.json").write_text(json.dumps(PRESETS, indent=1))
        (self.root / "CMakeLists.txt").write_text(LISTS.format(targets=" ".join(TARGETS)))
        (self.root / "fuzz").mkdir()
        (self.root / "fuzz" / "fuzz_alpha.dict").write_text('"word"\n')
        python = sys.executable
        for name in TARGETS:
            executable(self.root / "targets" / name, TARGET.format(python=python, name=name))
            seeds = self.root / "seeds" / name
            seeds.mkdir(parents=True)
            (seeds / "seed").write_text("s")
        executable(self.root / "targets" / "env_probe", ENV_PROBE.format(python=python))
        executable(self.root / "targets" / "hang", HANG.format(python=python))
        executable(self.root / "wrapper", WRAPPER.format(python=python))
        for toolchain in ("toolchain", "toolchain-nosym"):
            executable(self.root / toolchain / "bin" / "clang++", "#!/bin/sh\nexit 1\n")
        executable(self.root / "toolchain" / "bin" / "llvm-symbolizer", "#!/bin/sh\nexit 0\n")
        self.symbolizer = str(self.root / "toolchain" / "bin" / "llvm-symbolizer")
        self.state = self.root / "state"
        for preset in ("fuzz", "nosym", "plain", "hangs"):
            done = subprocess.run(
                ["cmake", "--preset", preset], cwd=self.root, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True,
            )  # fmt: skip
            if done.returncode != 0:
                raise RuntimeError(f"cmake --preset {preset} failed:\n{done.stdout}")

    def reset(self, behaviour=None):
        if self.state.exists():
            shutil.rmtree(self.state)
        self.state.mkdir()
        (self.state / "behaviour.json").write_text(json.dumps(behaviour or {}))

    def run(self, *arguments, extra_env=None, timeout=300):
        """A runner that outlives the timeout fails the test, by TimeoutExpired."""
        environment = {k: v for k, v in os.environ.items() if k not in CLEARED}
        environment["FAKE_FUZZ_STATE"] = str(self.state)
        environment.update(extra_env or {})
        start = time.monotonic()
        done = subprocess.run(
            [sys.executable, "-I", "scripts/run_tests.py", *arguments],
            cwd=self.root,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
        done.elapsed = time.monotonic() - start
        return done

    def calls(self, name=None):
        path = self.state / "calls.jsonl"
        if not path.exists():
            return []
        calls = [json.loads(line) for line in path.read_text().splitlines()]
        return [call for call in calls if name is None or call["name"] == name]

    def work(self, name):
        return self.root / "build" / "fuzz-smoke" / name


def load_runner():
    sys.dont_write_bytecode = True  # no scripts/__pycache__ in the tracked tree
    spec = importlib.util.spec_from_file_location("run_tests_under_test", RUNNER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def max_total_time(call):
    return [word for word in call["argv"] if word.startswith("-max_total_time=")]


class Units(unittest.TestCase):
    def test_the_slice_plan_fills_whole_slices_then_the_rest(self):
        runner = load_runner()
        cases = {
            (1800, 300): [300] * 6,
            (1000, 300): [300, 300, 300, 100],
            (301, 300): [300, 1],
            (300, 300): [300],
            (30, 300): [30],
            (7, 2): [2, 2, 2, 1],
            (1, 1): [1],
        }
        for (seconds, length), plan in cases.items():
            self.assertEqual(runner.slice_plan(seconds, length), plan, (seconds, length))
            self.assertEqual(sum(plan), seconds)

    def test_the_peak_memory_comes_from_final_stats_or_else_status_lines(self):
        runner = load_runner()
        text = "#2 NEW rss: 40Mb\n#9 pulse rss: 61Mb\nstat::peak_rss_mb:              58\n"
        self.assertEqual(runner.peak_rss(text), (58, "final stats"))
        self.assertEqual(runner.peak_rss("#2 NEW rss: 40Mb\n#9 rss: 61Mb\n"), (61, "status lines"))
        self.assertEqual(runner.peak_rss("no figures\n"), (None, None))

    def test_leak_detection_is_turned_on_only_on_macos_and_never_over_the_caller(self):
        runner = load_runner()
        environment, line = runner.leak_environment({}, "Darwin")
        self.assertEqual(environment["ASAN_OPTIONS"], "detect_leaks=1")
        self.assertEqual(
            environment["LSAN_OPTIONS"], f"suppressions={REPO}/scripts/lsan_suppressions.txt"
        )
        self.assertIn("leak detection: on", line)
        environment, _ = runner.leak_environment({"ASAN_OPTIONS": "abort_on_error=1"}, "Darwin")
        self.assertEqual(environment["ASAN_OPTIONS"], "abort_on_error=1:detect_leaks=1")
        environment, line = runner.leak_environment({"ASAN_OPTIONS": "detect_leaks=0"}, "Darwin")
        self.assertEqual(environment, {"ASAN_OPTIONS": "detect_leaks=0"})
        self.assertIn("as the caller's ASAN_OPTIONS sets it", line)
        environment, _ = runner.leak_environment({"LSAN_OPTIONS": "suppressions=x"}, "Darwin")
        self.assertEqual(environment["LSAN_OPTIONS"], "suppressions=x")
        self.assertEqual(runner.leak_environment({}, "Linux"), ({}, None))
        self.assertTrue((REPO / "scripts" / "lsan_suppressions.txt").is_file())


class Fuzz(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.project = Project("fuzz")

    def test_a_run_within_one_slice_is_one_invocation_with_the_old_arguments(self):
        project = self.project
        project.reset()
        done = project.run("fuzz", "--seconds", "3")
        self.assertEqual(done.returncode, 0, done.stdout)
        for name in TARGETS:
            calls = project.calls(name)
            self.assertEqual(len(calls), 1, done.stdout)
            work = project.work(name)
            expected = [
                f"-max_total_time=3",
                f"-artifact_prefix={work}{os.sep}",
                "-print_final_stats=1",
                str(work / "corpus"),
                str(project.root / "seeds" / name),
            ]
            if name == "aadp_fuzz_alpha":
                expected.insert(0, f"-dict={project.root / 'fuzz' / 'fuzz_alpha.dict'}")
            self.assertEqual(calls[0]["argv"], expected)
            self.assertIn(
                f"run_tests: fuzz {name}: exit 0, output in build/fuzz-smoke/{name}/output.log\n",
                done.stdout,
            )
        self.assertNotIn(" slice 1 of", done.stdout)
        self.assertIn("run_tests: 3 fuzz targets, 3 s each\n", done.stdout)
        self.assertTrue(done.stdout.endswith("run_tests: fuzz total=3 failed=0\n"), done.stdout)

    def test_each_target_gets_its_seconds_in_slices_of_at_most_the_slice_length(self):
        project = self.project
        for seconds, length, plan in (("5", "2", [2, 2, 1]), ("4", "2", [2, 2]), ("2", "5", [2])):
            project.reset()
            done = project.run("fuzz", "--seconds", seconds, "--slice", length)
            self.assertEqual(done.returncode, 0, done.stdout)
            for name in TARGETS:
                got = [max_total_time(call) for call in project.calls(name)]
                self.assertEqual(got, [[f"-max_total_time={s}"] for s in plan], done.stdout)
                if len(plan) > 1:
                    for number, s in enumerate(plan, 1):
                        self.assertIn(
                            f"run_tests: fuzz {name}: slice {number} of {len(plan)}, {s} s: "
                            f"exit 0, peak_rss_mb=30, output in build/fuzz-smoke/{name}/"
                            f"slice-{number}.log\n",
                            done.stdout,
                        )

    def test_the_slices_of_a_target_share_one_corpus_emptied_when_the_target_starts(self):
        project = self.project
        project.reset()
        stale = project.work("aadp_fuzz_beta") / "corpus" / "stale-unit"
        stale.parent.mkdir(parents=True, exist_ok=True)
        stale.write_text("old")
        done = project.run("fuzz", "--seconds", "3", "--slice", "1")
        self.assertEqual(done.returncode, 0, done.stdout)
        for name in TARGETS:
            calls = project.calls(name)
            self.assertEqual(len(calls), 3)
            self.assertEqual({call["corpus"] for call in calls}, {str(project.work(name) / "corpus")})
            for call in calls:
                earlier = [f"{name}-unit-{n}" for n in range(1, call["slice"])]
                self.assertEqual(call["found"], earlier, call)

    def test_a_failing_slice_fails_its_target_by_number_and_the_other_targets_still_run(self):
        project = self.project
        project.reset({"aadp_fuzz_alpha": {"2": {"exit": 71, "peak": 99}}})
        done = project.run("fuzz", "--seconds", "3", "--slice", "1")
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertEqual(len(project.calls("aadp_fuzz_alpha")), 2, done.stdout)
        self.assertEqual(len(project.calls("aadp_fuzz_beta")), 3, done.stdout)
        self.assertEqual(len(project.calls("aadp_fuzz_gamma")), 3, done.stdout)
        self.assertIn(
            "run_tests: fuzz aadp_fuzz_alpha: slice 2 of 3, 1 s: exit 71, peak_rss_mb=99, output in "
            "build/fuzz-smoke/aadp_fuzz_alpha/slice-2.log\n",
            done.stdout,
        )
        self.assertIn(
            "run_tests: fuzz aadp_fuzz_alpha: failed in slice 2 of 3: exit 71; slices 3 to 3 not "
            "run, 3 slices of 3 s, largest peak_rss_mb=99 (slice 2)\n",
            done.stdout,
        )
        self.assertIn("run_tests: fuzz aadp_fuzz_beta: passed, 3 slices of 3 s", done.stdout)
        self.assertTrue((project.work("aadp_fuzz_alpha") / "slice-2.log").is_file())
        self.assertTrue(done.stdout.endswith("run_tests: fuzz total=3 failed=1\n"), done.stdout)

    def test_a_slice_that_hangs_is_killed_with_its_children_and_recorded_as_timed_out(self):
        project = self.project
        project.reset({"aadp_fuzz_beta": {"1": {"hang": True}}})
        done = project.run("fuzz", "--seconds", "2", "--slice", "1", "--margin", "2", timeout=60)
        self.assertEqual(done.returncode, 1, done.stdout)
        # The limit is 3 s; three targets that do not hang take a second or two in all.
        self.assertLess(done.elapsed, 20, done.stdout)
        self.assertIn(
            "run_tests: fuzz aadp_fuzz_beta: slice 1 of 2, 1 s: timed out after 3 s, killed, "
            "peak_rss_mb=unknown",
            done.stdout,
        )
        self.assertIn(
            "run_tests: fuzz aadp_fuzz_beta: failed in slice 1 of 2: timed out after 3 s, killed; "
            "slices 2 to 2 not run",
            done.stdout,
        )
        self.assertEqual(len(project.calls("aadp_fuzz_gamma")), 2, done.stdout)
        child = int((project.state / "aadp_fuzz_beta.child").read_text())
        self.assertTrue(gone_within(child, 5), "the hanging slice's child outlived it")

    def test_every_slice_goes_through_the_wrapper(self):
        project = self.project
        project.reset()
        wrapper = str(project.root / "wrapper")
        done = project.run("fuzz", "--seconds", "3", "--slice", "1", "--wrapper", wrapper)
        self.assertEqual(done.returncode, 0, done.stdout)
        lines = (project.state / "wrapper.jsonl").read_text().splitlines()
        wrapped = [json.loads(line) for line in lines]
        self.assertEqual(len(wrapped), 9)
        for name in TARGETS:
            runs = [argv for argv in wrapped if argv[0] == str(project.root / "targets" / name)]
            self.assertEqual([argv[1:] for argv in runs], [c["argv"] for c in project.calls(name)])
        self.assertIn(f"run_tests: each slice through {wrapper}", done.stdout)

    def test_the_limit_does_not_count_the_wrappers_wait(self):
        project = self.project
        project.reset()
        done = project.run(
            "fuzz", "--seconds", "1", "--margin", "1", "--only", "aadp_fuzz_alpha", "--wrapper",
            str(project.root / "wrapper"), extra_env={"FAKE_WRAPPER_SLEEP": "4"},
        )  # fmt: skip
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertGreater(done.elapsed, 4)
        self.assertIn("run_tests: fuzz aadp_fuzz_alpha: exit 0", done.stdout)

    def test_the_wrappers_wait_has_its_own_bound(self):
        project = self.project
        project.reset()
        done = project.run(
            "fuzz", "--seconds", "1", "--only", "aadp_fuzz_alpha", "--wrapper",
            str(project.root / "wrapper"), "--wrapper-wait", "2",
            extra_env={"FAKE_WRAPPER_SLEEP": "60"},
        )  # fmt: skip
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertLess(done.elapsed, 20, done.stdout)
        self.assertIn(
            "run_tests: fuzz aadp_fuzz_alpha: timed out waiting for the wrapper, no output in 2 s, "
            "killed",
            done.stdout,
        )
        self.assertEqual(project.calls(), [])

    def test_each_slice_and_each_target_print_their_peak_memory(self):
        project = self.project
        project.reset(
            {
                "aadp_fuzz_gamma": {
                    "1": {"peak": 100},
                    "2": {"peak": 250},
                    "3": {"final": False, "status_rss": [40, 77, 60]},
                }
            }
        )
        done = project.run("fuzz", "--seconds", "3", "--slice", "1", "--only", "aadp_fuzz_gamma")
        self.assertEqual(done.returncode, 0, done.stdout)
        for number, text in ((1, "peak_rss_mb=100"), (2, "peak_rss_mb=250"),
                             (3, "peak_rss_mb=77 (from status lines)")):  # fmt: skip
            self.assertIn(
                f"run_tests: fuzz aadp_fuzz_gamma: slice {number} of 3, 1 s: exit 0, {text}, ",
                done.stdout,
            )
        self.assertIn(
            "run_tests: fuzz aadp_fuzz_gamma: passed, 3 slices of 3 s, largest peak_rss_mb=250 "
            "(slice 2)\n",
            done.stdout,
        )
        project.reset()
        done = project.run("fuzz", "--seconds", "2", "--only", "aadp_fuzz_gamma")
        self.assertIn(
            "run_tests: fuzz aadp_fuzz_gamma: passed, 1 slice of 2 s, largest peak_rss_mb=30 "
            "(slice 1)\n",
            done.stdout,
        )

    def test_only_runs_the_named_targets_and_refuses_an_unknown_one(self):
        project = self.project
        project.reset()
        done = project.run("fuzz", "--seconds", "1", "--only", "aadp_fuzz_beta")
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertEqual({call["name"] for call in project.calls()}, {"aadp_fuzz_beta"})
        self.assertIn("run_tests: --only: 1 of them, aadp_fuzz_beta\n", done.stdout)
        self.assertTrue(done.stdout.endswith("run_tests: fuzz total=1 failed=0\n"), done.stdout)
        done = project.run("fuzz", "--seconds", "1", "--only", "aadp_fuzz_delta")
        self.assertEqual(done.returncode, 2, done.stdout)
        self.assertIn("no such fuzz target: aadp_fuzz_delta", done.stdout)


class Symbolizer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.project = Project("symbolizer")

    def symbolizers(self, call):
        return [call["env"][name] for name in CLEARED[:3]]

    def test_fuzz_runs_get_the_symbolizer_beside_the_build_trees_compiler(self):
        project = self.project
        project.reset()
        done = project.run("fuzz", "--seconds", "1")
        self.assertEqual(done.returncode, 0, done.stdout)
        for call in project.calls():
            self.assertEqual(self.symbolizers(call), [project.symbolizer] * 3)
        self.assertIn(
            f"run_tests: symbolizer: {project.symbolizer}, beside the compiler of build/fuzz\n",
            done.stdout,
        )

    def test_a_variable_the_caller_set_is_left_alone(self):
        project = self.project
        project.reset()
        done = project.run(
            "fuzz", "--seconds", "1", extra_env={"ASAN_SYMBOLIZER_PATH": "/caller/symbolizer"}
        )
        self.assertEqual(done.returncode, 0, done.stdout)
        for call in project.calls():
            expected = ["/caller/symbolizer", project.symbolizer, project.symbolizer]
            self.assertEqual(self.symbolizers(call), expected)
        self.assertIn("ASAN_SYMBOLIZER_PATH as the caller set it\n", done.stdout)

    def test_with_no_symbolizer_beside_the_compiler_the_environment_is_left_alone(self):
        project = self.project
        project.reset()
        done = project.run("fuzz", "--seconds", "1", "--preset", "nosym")
        self.assertEqual(done.returncode, 0, done.stdout)
        for call in project.calls():
            self.assertEqual(self.symbolizers(call), [None] * 3)
        compiler = project.root / "toolchain-nosym" / "bin" / "clang++"
        self.assertIn(
            f"run_tests: symbolizer: no llvm-symbolizer beside {compiler}, environment left alone",
            done.stdout,
        )

    def test_ctest_runs_get_the_symbolizer_too(self):
        project = self.project
        project.reset()
        done = project.run("ctest", "fuzz", "--junit", str(project.state / "junit.xml"))
        self.assertEqual(done.returncode, 0, done.stdout)
        probe = json.loads((project.state / "probe.json").read_text())
        self.assertEqual(list(probe.values()), [project.symbolizer] * 3)
        self.assertIn(f"run_tests: symbolizer: {project.symbolizer}, beside", done.stdout)
        done = project.run("ctest", "nosym", "--junit", str(project.state / "junit.xml"))
        self.assertEqual(done.returncode, 0, done.stdout)
        probe = json.loads((project.state / "probe.json").read_text())
        self.assertEqual(list(probe.values()), [None] * 3)


class CtestTimeout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.project = Project("ctest-timeout")

    def test_a_sanitizer_build_gets_a_per_test_timeout_and_a_plain_build_does_not(self):
        project = self.project
        project.reset()
        done = project.run("ctest", "fuzz", "--junit", str(project.state / "junit.xml"))
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertIn(
            "run_tests: per-test timeout: 1500 s for a test with no TIMEOUT of its own, in a "
            "sanitizer build\n",
            done.stdout,
        )
        done = project.run("ctest", "plain", "--junit", str(project.state / "junit.xml"))
        self.assertEqual(done.returncode, 0, done.stdout)
        self.assertIn(
            "run_tests: per-test timeout: ctest's default, not a sanitizer build\n", done.stdout
        )

    def test_a_test_that_hangs_fails_at_the_timeout_and_its_child_goes_with_it(self):
        project = self.project
        project.reset()
        done = project.run(
            "ctest", "hangs", "--jobs", "4", "--test-timeout", "3", "--junit",
            str(project.state / "junit.xml"),
        )  # fmt: skip
        self.assertEqual(done.returncode, 1, done.stdout)
        self.assertLess(done.elapsed, 60, done.stdout)
        self.assertIn("run_tests: failed: hang\n", done.stdout)
        self.assertIn("run_tests: hangs total=5 passed=4 failed=1", done.stdout)
        child = int((project.state / "hang.child").read_text())
        self.assertTrue(gone_within(child, 10), "the hanging test's child outlived it")


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1].startswith("-"):
        print("usage: run_tests_test.py <scratch-dir> [unittest arguments]", file=sys.stderr)
        sys.exit(2)
    SCRATCH = Path(sys.argv[1]).resolve()
    SCRATCH.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
