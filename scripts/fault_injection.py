#!/usr/bin/env python3
"""Proves that each oracle and check can fail, by injecting one bug at a time into a copy.

    python3 -I scripts/fault_injection.py [--only <id,...>] [--kind oracle|check|fuzz]
        [--jobs <n>] [--source <dir>] [--output <file>] [--list]
        [--manifest <file>] [--build-root <dir>] [--scope-note <text>]

Run it with the toolchain in the environment (CC, CXX, SDKROOT, PATH). Standard library only.
Exit codes: 0 every selected fault was caught, 1 otherwise, 2 could not run.

For each fault: the unmutated copy must build and pass, the edits are applied, the mutated copy
is built and run, and the edited files go back to their pristine bytes. Verdicts:
  caught           the fault was detected for the named reason
  not-caught       the build or command did not show the named reason: it passed, timed out,
                   failed for another reason, or (detect test) did not compile
  not-applied      an edit could not be applied, so nothing was tested
  baseline-failed  the unmutated copy failed, or the reason regex already matches its output,
                   so the fault proves nothing
A verdict other than caught is a failure. Raw output: <build root>/logs/<id>-<stage>.txt.

Where things live. The working tree is never touched. <build root> defaults to build/fault:
  pristine/   a fresh export of the code under test, rebuilt on every run
  copy/       the tree that gets mutated and built. Synced from pristine by content at the start
              of a run, so a run that was killed leaves nothing behind and unchanged files keep
              their timestamps, so a fault rebuilds only what it touched
  copy/build/<preset>/  one build directory per CMake preset, where the presets put it, so that
              `ctest --preset`, run_tests.py, run_clang_tidy.sh and check.sh work in the copy
              exactly as they do in the repo
  copy/build/tools      a link to build/tools of the tree the code came from, when it exists
  _deps/      FetchContent sources, keyed by a hash of cmake/AadpDependencies.cmake, seeded from
              any sibling build directory holding the same pinned archive, so nothing is fetched
              twice
  logs/       raw output of every step

The code under test comes from `git archive HEAD` of the repo holding this script. An export with
no .git, as on the Linux host, passes --source <dir>; it is copied, its COMMIT file names the
commit, and the build root defaults to build/fault next to <dir>. The manifest comes from the same
place as the code, so uncommitted edits to it have no effect without --manifest.

The copy has no .git and no data/, which suits a test of the code. A check that reads either must
ask for it with the git or data field, and then gets the same thing for its baseline and for the
mutated run. Without git, `git ls-files` in the copy walks up to the real repo, which ignores
build/, and lists nothing, so check_format.sh and scan_repo.py could not run at all.

Manifest, faults/manifest.json: {"schema": 1, "faults": [<fault>, ...]}. A fault has these fields,
all required unless marked:
  id         lower-case, digits and hyphens, unique
  kind       "oracle", "check" or "fuzz"
  covers     what the fault attacks, a few words
  bug        one sentence saying what the bug is
  edits      list of edits, applied in order. Paths are relative to the repo root, and a path that
             resolves outside the copy is refused. Each is one of
               {"file": <path>, "find": <text>, "replace": <text>}, where find must match exactly
               once, overlapping matches counted, or the fault is not-applied
               {"file": <path>, "append": <text>}
               {"file": <path>, "append_line_of": <path>, "line": <n>}, which appends line n,
               counted from 1, of another file in the copy. For a line that must not be written
               in the manifest, such as a row of the raw data
  preset     CMake configure preset, which also names the build directory
  targets    list of build targets. Empty for a check that needs no build, such as a script
  command    argv run from the root of the copy. "{build}" expands to the build directory and
             "{jobs}" to --jobs, which AADP_JOBS also carries to a nested check.sh, so that a
             fault can never exceed a host's job limit. No shell. Optional when detect is "build";
             if present it must pass on the baseline
  detect     "test": the build must succeed, and the command must fail with reason matched in its
             output. "build": the build must fail with reason matched in its output
  reason     Python regex, searched with re.MULTILINE. It must name the test or the reason, and it
             must not match the baseline's output
  timeout_s  limit on each run of the command. Builds have a fixed limit, BUILD_TIMEOUT_S
And these, all optional:
  baseline_exit  the exit a passing baseline gives, default 0. check.sh --only exits 3 when
             nothing failed. A mutated run with this same exit is not caught
  mutated_exit   the exact exit the mutated run must give, for a check whose exit code has a
             documented meaning. Any other non-baseline exit is not caught
  configure  list of presets to configure, for a check with no targets that builds or reads the
             build tree itself. The preset of a fault with targets is always configured
  git        true: the copy is a git work tree for this fault, tracking what the export holds. A
             repo of its own, so the real index is not involved
  data       true: the copy has its own copy of data/ for this fault, from the tree the code came
             from. A check that writes there cannot reach the real files. A missing data/ is
             baseline-failed, so a check cannot pass by having nothing to read
  output_files   list of globs relative to the copy. Files a check writes rather than prints, such
             as check.sh's step logs. Deleted before each run and searched with its output

To add a fault: add an entry, commit it, and run --only <id>. No other file changes.

The output has no absolute paths into a home directory, host names or user names, because it gets
committed. Anything that would leak after scrubbing makes the run refuse to write it. Its scope
line says how much of the manifest the run covered, names whatever it left out, and carries
--scope-note, the reason, so a subset cannot read as the full set.
"""

import argparse
import fcntl
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAX_JOBS = 6
BUILD_TIMEOUT_S = 1800
REASON_CHARS = 200
KINDS = ("oracle", "check", "fuzz")
DETECTS = ("test", "build")
VERDICTS = ("caught", "not-caught", "not-applied", "baseline-failed")
COLUMNS = "id | covers | baseline | mutated_exit | reason | verdict"
FAULT_KEYS = {
    "id", "kind", "covers", "bug", "edits", "preset", "targets", "command", "detect", "reason",
    "timeout_s",
}  # fmt: skip
OPTIONAL_KEYS = {"baseline_exit", "mutated_exit", "configure", "git", "data", "output_files"}
NAME = re.compile(r"[a-z0-9][a-z0-9-]*\Z")
URL_HASH = re.compile(r"URL_HASH\s+SHA256=([0-9a-fA-F]{64})")
LEAK = re.compile(
    r"(?<![A-Za-z0-9_.~-])/(?:Users|home|root)/[A-Za-z0-9._-]+"
    r"|(?<![\d.])\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}(?!\d|\.\d)"
)


class CannotRun(Exception):
    pass


class NotApplied(Exception):
    pass


class NoFixture(Exception):
    pass


def require(condition, message):
    if not condition:
        raise CannotRun(message)


def is_whole_number(value):
    return isinstance(value, int) and not isinstance(value, bool)


def inside_copy(fault_id, path):
    parts = Path(path).parts
    require(
        parts and not Path(path).is_absolute() and ".." not in parts,
        f"{fault_id}: refusing path outside the copy: {path}",
    )


def validate_edit(fault_id, edit):
    keys = set(edit) if isinstance(edit, dict) else set()
    require(
        keys
        in ({"file", "find", "replace"}, {"file", "append"}, {"file", "append_line_of", "line"}),
        f"{fault_id}: an edit needs file with find and replace, with append, or with "
        "append_line_of and line",
    )
    line = edit.get("line", 1)
    require(is_whole_number(line) and line >= 1, f"{fault_id}: bad line number")
    require(
        all(isinstance(value, str) for key, value in edit.items() if key != "line"),
        f"{fault_id}: edit fields must be text",
    )
    inside_copy(fault_id, edit["file"])
    if "find" in edit:
        require(edit["find"], f"{fault_id}: empty find")
        require(edit["find"] != edit["replace"], f"{fault_id}: find equals replace")
    elif "append" in edit:
        require(edit["append"], f"{fault_id}: empty append")
    else:
        inside_copy(fault_id, edit["append_line_of"])


def validate_optional(fault_id, fault):
    for key in ("baseline_exit", "mutated_exit"):
        require(is_whole_number(fault.get(key, 0)), f"{fault_id}: bad {key}")
    require(
        fault.get("mutated_exit", 1) != fault.get("baseline_exit", 0),
        f"{fault_id}: mutated_exit equals the baseline's exit",
    )
    for key in ("git", "data"):
        require(isinstance(fault.get(key, False), bool), f"{fault_id}: bad {key}")
    for key in ("configure", "output_files"):
        value = fault.get(key, [])
        require(
            isinstance(value, list) and all(isinstance(v, str) and v for v in value),
            f"{fault_id}: {key} must be a list of text",
        )
    for preset in fault.get("configure", []):
        require(NAME.match(preset), f"{fault_id}: bad preset in configure")
    for pattern in fault.get("output_files", []):
        inside_copy(fault_id, pattern)


def validate_fault(fault, seen):
    require(isinstance(fault, dict), "a fault must be an object")
    fault_id = fault.get("id", "<no id>")
    require(isinstance(fault_id, str) and NAME.match(fault_id), f"bad fault id: {fault_id}")
    require(fault_id not in seen, f"duplicate fault id: {fault_id}")
    seen.add(fault_id)
    required = FAULT_KEYS - ({"command"} if fault.get("detect") == "build" else set())
    known = FAULT_KEYS | OPTIONAL_KEYS
    require(
        required <= set(fault) <= known,
        f"{fault_id}: fields differ from the schema: "
        f"{sorted((required - set(fault)) | (set(fault) - known))}",
    )
    validate_optional(fault_id, fault)
    require(fault["kind"] in KINDS, f"{fault_id}: bad kind")
    require(fault["detect"] in DETECTS, f"{fault_id}: bad detect")
    for key in ("covers", "bug", "preset", "reason"):
        require(isinstance(fault[key], str) and fault[key], f"{fault_id}: bad {key}")
    require(NAME.match(fault["preset"]), f"{fault_id}: bad preset")
    for key, value in (("targets", fault["targets"]), ("command", fault.get("command", ["-"]))):
        require(
            isinstance(value, list) and all(isinstance(v, str) and v for v in value),
            f"{fault_id}: {key} must be a list of text",
        )
    require(fault.get("command", ["-"]), f"{fault_id}: empty command")
    require(fault["detect"] == "test" or fault["targets"], f"{fault_id}: build mode, no targets")
    timeout = fault["timeout_s"]
    require(
        isinstance(timeout, (int, float)) and not isinstance(timeout, bool) and timeout > 0,
        f"{fault_id}: bad timeout_s",
    )
    require(
        isinstance(fault["edits"], list) and fault["edits"],
        f"{fault_id}: edits must be a non-empty list",
    )
    for edit in fault["edits"]:
        validate_edit(fault_id, edit)
    try:
        re.compile(fault["reason"])
    except re.error as error:
        raise CannotRun(f"{fault_id}: bad reason regex: {error}") from error


def parse_manifest(text):
    try:
        manifest = json.loads(text)
    except ValueError as error:
        raise CannotRun(f"manifest is not JSON: {error}") from error
    require(isinstance(manifest, dict) and manifest.get("schema") == 1, "manifest schema is not 1")
    require(isinstance(manifest.get("faults"), list), "manifest has no faults list")
    seen = set()
    for fault in manifest["faults"]:
        validate_fault(fault, seen)
    return manifest["faults"]


def git(*arguments, **options):
    return subprocess.run(
        ["git", "-C", str(REPO), *arguments], check=True, stdout=subprocess.PIPE, **options
    ).stdout


def source_commit(args):
    if args.source:
        marker = args.source / "COMMIT"
        text = marker.read_text().strip() if marker.is_file() else ""
        return text if re.fullmatch(r"[0-9a-f]{7,64}", text) else "unknown"
    try:
        return git("rev-parse", "HEAD", stderr=subprocess.PIPE).decode().strip()
    except (OSError, subprocess.CalledProcessError) as error:
        raise CannotRun(f"no commit here, and no --source: {error}") from error


def read_manifest_text(args, commit):
    try:
        if args.manifest:
            return args.manifest.read_text()
        if args.source:
            return (args.source / "faults" / "manifest.json").read_text()
        return git("show", f"{commit}:faults/manifest.json", stderr=subprocess.PIPE).decode()
    except (OSError, subprocess.CalledProcessError) as error:
        raise CannotRun(f"cannot read the manifest: {error}") from error


class Layout:
    def __init__(self, build_root, origin):
        self.root = build_root
        self.origin = origin  # The tree the code came from, for its data/ and build/tools.
        self.pristine = build_root / "pristine"
        self.copy = build_root / "copy"
        self.logs = build_root / "logs"
        self.deps = build_root / "_deps"

    def build_dir(self, preset):
        return self.copy / "build" / preset


def fresh_write(path, data):
    """Writes with a new timestamp, so that ninja rebuilds whatever depends on path. Going back to
    the old timestamp on a restore would leave objects built from the mutation looking current."""
    path.write_bytes(data)
    now = time.time_ns()
    os.utime(path, ns=(now, now))


def clear_dir(path):
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)


def export_pristine(args, commit, layout):
    clear_dir(layout.pristine)
    if args.source:
        shutil.copytree(
            args.source,
            layout.pristine,
            symlinks=True,
            ignore=lambda folder, names: [
                name for name in names if name in (".git", "build") and Path(folder) == args.source
            ],
        )
        return
    layout.pristine.mkdir(parents=True)
    archive = subprocess.Popen(
        ["git", "-C", str(REPO), "archive", "--format=tar", commit], stdout=subprocess.PIPE
    )
    unpack = subprocess.run(["tar", "-xf", "-", "-C", str(layout.pristine)], stdin=archive.stdout)
    archive.stdout.close()
    require(archive.wait() == 0 and unpack.returncode == 0, "could not export the code under test")


def relative_files(root):
    """Everything but a build/ at the top, which in the copy holds the build trees."""
    found = set()
    for folder, directories, files in os.walk(root):
        if Path(folder) == root and "build" in directories:
            directories.remove("build")
        for name in files + [d for d in directories if (Path(folder) / d).is_symlink()]:
            found.add((Path(folder) / name).relative_to(root))
    return found


def same_entry(a, b):
    if a.is_symlink() or b.is_symlink():
        return a.is_symlink() and b.is_symlink() and os.readlink(a) == os.readlink(b)
    return b.is_file() and a.stat().st_mode == b.stat().st_mode and a.read_bytes() == b.read_bytes()


def restore_entry(layout, relative):
    source, target = layout.pristine / relative, layout.copy / relative
    if source.is_symlink():
        clear_dir(target)
        target.symlink_to(os.readlink(source))
        return
    if target.is_symlink():
        target.unlink()
    target.parent.mkdir(parents=True, exist_ok=True)
    fresh_write(target, source.read_bytes())
    shutil.copymode(source, target)


def sync_copy(layout):
    layout.copy.mkdir(parents=True, exist_ok=True)
    remove_fixture(layout)  # A killed run can leave them.
    wanted = relative_files(layout.pristine)
    for relative in relative_files(layout.copy) - wanted:
        (layout.copy / relative).unlink()
    for relative in sorted(wanted):
        if not same_entry(layout.pristine / relative, layout.copy / relative):
            restore_entry(layout, relative)
    tools = layout.origin / "build" / "tools"
    link = layout.copy / "build" / "tools"
    clear_dir(link)
    if tools.is_dir():
        link.parent.mkdir(exist_ok=True)
        link.symlink_to(tools)


def make_git_tree(layout):
    """Tracks the export's files by name, so that .gitignore cannot drop a tracked one and data/
    and build/ stay untracked. The repo is the copy's own, so no index but its own is written."""
    root = str(layout.copy)
    names = "\0".join(str(relative) for relative in sorted(relative_files(layout.pristine)))
    for command, stdin in (
        (["init", "-q"], None),
        (["add", "-f", "--pathspec-from-file=-", "--pathspec-file-nul"], names.encode()),
    ):
        done = subprocess.run(
            ["git", "-C", root, *command], input=stdin, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )  # fmt: skip
        require(done.returncode == 0, f"cannot make a git tree: {done.stdout.decode()}")


def install_fixture(layout, fault):
    if fault.get("data"):
        origin = layout.origin / "data"
        if not origin.is_dir():
            raise NoFixture(f"no data/ in {origin.parent.name}: run scripts/fetch_data.sh")
        shutil.copytree(origin, layout.copy / "data")
    if fault.get("git"):
        make_git_tree(layout)


def remove_fixture(layout):
    for name in (".git", "data"):
        if not (layout.pristine / name).exists():
            clear_dir(layout.copy / name)


def run_logged(command, cwd, log, timeout):
    """Returns (exit code, output). The exit code is None on a timeout, and a signal is negative.
    The child leads its own process group, so a timeout takes its children too."""
    environment = dict(os.environ, GTEST_COLOR="no")
    with open(log, "wb") as sink:
        try:
            child = subprocess.Popen(
                command,
                cwd=cwd,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=sink,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        except OSError as error:
            sink.write(f"cannot execute {command[0]}: {error}\n".encode())
            code = 127
        else:
            try:
                code = child.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
                code = None
    return code, Path(log).read_text(errors="replace")


def deps_cache(layout):
    pins = layout.copy / "cmake" / "AadpDependencies.cmake"
    digest = hashlib.sha256(pins.read_bytes() if pins.is_file() else b"").hexdigest()
    return layout.deps / digest[:16]


def seed_deps(layout):
    """Copies sources of the same pinned archive from any sibling build directory."""
    pins = layout.copy / "cmake" / "AadpDependencies.cmake"
    if not pins.is_file():
        return
    pinned = pins.read_text(errors="replace")
    cache = deps_cache(layout)
    for subbuild in sorted(layout.root.parent.glob("*/_deps/*-subbuild/CMakeLists.txt")):
        name = subbuild.parent.name[: -len("-subbuild")]
        origin = subbuild.parent.parent / f"{name}-src"
        if (cache / f"{name}-src").exists() or not origin.is_dir():
            continue
        try:
            found = URL_HASH.search(subbuild.read_text(errors="replace"))
        except OSError:  # Another job is replacing that build directory.
            continue
        if found and found.group(1) in pinned:
            cache.mkdir(parents=True, exist_ok=True)
            shutil.copytree(origin, cache / f"{name}-src", symlinks=True)


def harvest_deps(layout, build_dir):
    cache = deps_cache(layout)
    for origin in sorted((build_dir / "_deps").glob("*-src")):
        if not (cache / origin.name).exists():
            cache.mkdir(parents=True, exist_ok=True)
            shutil.copytree(origin, cache / origin.name, symlinks=True)


def deps_arguments(layout):
    arguments = []
    for folder in sorted(deps_cache(layout).glob("*-src")):
        name = folder.name[: -len("-src")].upper()
        arguments.append(f"-DFETCHCONTENT_SOURCE_DIR_{name}={folder}")
    return arguments


def configure(layout, preset):
    """Once per preset per run. Failing here means no fault could be judged."""
    build_dir = layout.build_dir(preset)
    seed_deps(layout)
    log = layout.logs / f"configure-{preset}.txt"
    command = ["cmake", "--preset", preset, "-B", str(build_dir), *deps_arguments(layout)]
    code, _ = run_logged(command, layout.copy, log, BUILD_TIMEOUT_S)
    require(code == 0, f"cmake --preset {preset} failed: see {log}")
    harvest_deps(layout, build_dir)


def build(layout, fault, stage, jobs):
    if not fault["targets"]:
        return 0, ""
    command = ["cmake", "--build", str(layout.build_dir(fault["preset"])), "--target"]
    command += [*fault["targets"], "--parallel", str(jobs)]
    log = layout.logs / f"{fault['id']}-{stage}-build.txt"
    return run_logged(command, layout.copy, log, BUILD_TIMEOUT_S)


def output_files(layout, fault):
    return [
        path
        for pattern in fault.get("output_files", [])
        for path in sorted(layout.copy.glob(pattern))
        if path.is_file()
    ]


def run_command(layout, fault, stage, jobs):
    build_dir = str(layout.build_dir(fault["preset"]))
    command = [
        part.replace("{build}", build_dir).replace("{jobs}", str(jobs)) for part in fault["command"]
    ]
    log = layout.logs / f"{fault['id']}-{stage}-run.txt"
    for stale in output_files(layout, fault):
        stale.unlink()
    code, output = run_logged(command, layout.copy, log, fault["timeout_s"])
    extra = "".join(path.read_text(errors="replace") + "\n" for path in output_files(layout, fault))
    if extra:
        with open(log, "a") as sink:
            sink.write(extra)
    return code, output + extra


def count_matches(text, find):
    """Overlapping matches count, since replacing one of them would be ambiguous."""
    count = 0
    start = text.find(find)
    while start != -1:
        count += 1
        start = text.find(find, start + 1)
    return count


def edit_target(layout, relative):
    copy_root = layout.copy.resolve()
    target = (layout.copy / relative).resolve()
    if not target.is_relative_to(copy_root) or target == copy_root:
        raise NotApplied(f"{relative} resolves outside the copy")
    if not target.is_file():
        raise NotApplied(f"{relative} does not exist")
    return target, target.relative_to(copy_root)


def line_of(layout, edit):
    source, _ = edit_target(layout, edit["append_line_of"])
    lines = source.read_bytes().splitlines()
    if edit["line"] > len(lines):
        raise NotApplied(f"{edit['append_line_of']} has {len(lines)} lines")
    return lines[edit["line"] - 1] + b"\n"


def apply_edits(layout, fault, touched):
    for edit in fault["edits"]:
        target, relative = edit_target(layout, edit["file"])
        touched.add(relative)
        data = target.read_bytes()
        if "find" in edit:
            find = edit["find"].encode()
            count = count_matches(data, find)
            if count != 1:
                raise NotApplied(f"find matched {count} times in {edit['file']}")
            data = data.replace(find, edit["replace"].encode())
        elif "append" in edit:
            data += edit["append"].encode()
        else:
            data += line_of(layout, edit)
        fresh_write(target, data)


def first_error_line(output):
    for line in output.splitlines():
        if "error:" in line:
            return line.strip()
    lines = output.strip().splitlines()
    return lines[-1].strip() if lines else "no output"


def judge(fault, reason, build_code, build_output, run_code, run_output):
    """Returns (mutated exit, reason text, verdict) for a fault that was applied."""
    timeout = fault["timeout_s"]
    if fault["detect"] == "build":
        if build_code is None:
            return "timeout", f"build timed out after {BUILD_TIMEOUT_S} s", "not-caught"
        if build_code == 0:
            return "0", "build succeeded", "not-caught"
        match = reason.search(build_output)
        if match:
            return str(build_code), match.group(0), "caught"
        return str(build_code), f"another build failure: {first_error_line(build_output)}", (
            "not-caught"
        )
    if build_code is None:
        return "timeout", f"build timed out after {BUILD_TIMEOUT_S} s", "not-caught"
    if build_code != 0:
        detail = first_error_line(build_output)
        return "build-failed", f"did not compile, expected a test failure: {detail}", "not-caught"
    if run_code is None:
        return "timeout", f"timed out after {timeout} s", "not-caught"
    if run_code == fault.get("baseline_exit", 0):
        return str(run_code), "command passed", "not-caught"
    if run_code != fault.get("mutated_exit", run_code):
        return str(run_code), f"expected exit {fault['mutated_exit']}", "not-caught"
    match = reason.search(run_output)
    if match:
        return str(run_code), match.group(0), "caught"
    return str(run_code), "failed, but not for the named reason", "not-caught"


def run_baseline(layout, fault, reason, jobs):
    """Returns None if the baseline passed, or why it did not."""
    code, output = build(layout, fault, "baseline", jobs)
    if code != 0:
        return f"baseline build failed: {first_error_line(output)}"
    if fault["detect"] == "build" and reason.search(output):
        return "the reason regex matches the passing baseline build"
    if "command" not in fault:
        return None
    code, output = run_command(layout, fault, "baseline", jobs)
    if code is None:
        return f"baseline timed out after {fault['timeout_s']} s"
    if code != fault.get("baseline_exit", 0):
        return f"baseline command exited {code}"
    if reason.search(output):
        return "the reason regex matches the passing baseline"
    return None


def run_fault(layout, fault, jobs):
    """Returns (baseline, mutated exit, reason text, verdict)."""
    reason = re.compile(fault["reason"], re.MULTILINE)
    touched = set()
    try:
        install_fixture(layout, fault)
        problem = run_baseline(layout, fault, reason, jobs)
        if problem:
            return "fail", "-", problem, "baseline-failed"
        apply_edits(layout, fault, touched)
        build_code, build_output = build(layout, fault, "mutated", jobs)
        run_code, run_output = None, ""
        if fault["detect"] == "test" and build_code == 0:
            run_code, run_output = run_command(layout, fault, "mutated", jobs)
        mutated, text, verdict = judge(fault, reason, build_code, build_output, run_code, run_output)
        return "pass", mutated, text, verdict
    except NoFixture as error:
        return "fail", "-", str(error), "baseline-failed"
    except NotApplied as error:
        return "pass", "-", str(error), "not-applied"
    finally:
        for relative in touched:
            restore_entry(layout, relative)
        remove_fixture(layout)


class Scrubber:
    def __init__(self, layout):
        replacements = [
            (str(layout.copy.resolve()), "<copy>"),
            (str(layout.root.resolve()), "<build>"),
            (str(layout.copy), "<copy>"),
            (str(layout.root), "<build>"),
            (str(REPO), "<repo>"),
            (str(Path.home()), "<home>"),
        ]
        self.paths = [(old, new) for old, new in replacements if len(old) > 1]
        names = {os.environ.get("USER", ""), os.environ.get("LOGNAME", "")}
        names.add(Path.home().name)
        hostname = socket.gethostname()
        names.update({hostname, hostname.split(".")[0]})
        names = sorted((n for n in names if len(n) >= 3), key=len, reverse=True)
        self.names = re.compile("|".join(map(re.escape, names)), re.IGNORECASE) if names else None

    def __call__(self, text):
        for old, new in self.paths:
            text = text.replace(old, new)
        return self.names.sub("<name>", text) if self.names else text


def field(text):
    return " ".join(text.split()).replace("|", "/")


def format_row(fault, baseline, mutated, reason, verdict):
    reason = field(reason)
    if len(reason) > REASON_CHARS:
        reason = reason[: REASON_CHARS - 3] + "..."
    return " | ".join(
        (fault["id"], field(fault["covers"]), baseline, mutated, reason or "-", verdict)
    )


def compiler_version():
    compiler = shutil.which(os.environ.get("CXX", "c++").split()[0])
    if not compiler:
        return "unknown"
    version = subprocess.run([compiler, "--version"], stdout=subprocess.PIPE, text=True)
    lines = version.stdout.splitlines()
    return lines[0].strip() if lines else "unknown"


def cpu_model():
    if platform.system() == "Darwin":
        command = ["sysctl", "-n", "machdep.cpu.brand_string"]
        result = subprocess.run(command, stdout=subprocess.PIPE, text=True)
        return result.stdout.strip()
    try:
        text = Path("/proc/cpuinfo").read_text(errors="replace")
    except OSError:
        return ""
    found = re.search(r"^model name\s*:\s*(.+)$", text, re.MULTILINE)
    return found.group(1).strip() if found else ""


def host_label():
    """The same labels as src/platform/host_label.cpp."""
    model = cpu_model()
    if platform.system() == "Darwin" and model == "Apple M4 Max":
        return "mac-m4max"
    if platform.system() == "Linux" and re.match(r"AMD Ryzen Embedded V1605B( |$)", model):
        return "linux-zen1"
    return "other"


def scope_line(faults, selected, note):
    if len(selected) == len(faults):
        scope = f"all {len(faults)} faults"
    else:
        chosen = {fault["id"] for fault in selected}
        left_out = ", ".join(fault["id"] for fault in faults if fault["id"] not in chosen)
        scope = f"subset, {len(selected)} of {len(faults)} faults, leaving out {left_out}"
    return f"scope: {scope}" + (f". {field(note)}" if note else "")


def header_lines(commit, faults, selected, args):
    return [
        "aadp-fault-injection 1",
        "claim: none",
        f"commit: {commit}",
        f"host_label: {host_label()}",
        f"compiler: {compiler_version()}",
        f"utc_time: {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}",
        f"jobs: {args.jobs}",
        f"selected: {len(selected)}",
        scope_line(faults, selected, args.scope_note),
        f"columns: {COLUMNS}",
    ]


def select_faults(faults, args):
    selected = faults
    if args.only:
        wanted = [name for part in args.only for name in part.split(",") if name]
        known = {fault["id"] for fault in faults}
        unknown = [name for name in wanted if name not in known]
        require(not unknown, f"no such fault: {', '.join(unknown)}")
        selected = [fault for fault in selected if fault["id"] in wanted]
    if args.kind:
        selected = [fault for fault in selected if fault["kind"] == args.kind]
    require(selected, "no fault selected")
    return selected


def default_build_root(args):
    return (args.source.parent if args.source else REPO) / "build" / "fault"


def run_all(faults, selected, layout, commit, args):
    scrub = Scrubber(layout)
    lines = [scrub(line) for line in header_lines(commit, faults, selected, args)]
    print("\n".join(lines), flush=True)
    export_pristine(args, commit, layout)
    sync_copy(layout)
    counts = dict.fromkeys(VERDICTS, 0)
    presets = {fault["preset"] for fault in selected if fault["targets"]}
    presets.update(preset for fault in selected for preset in fault.get("configure", []))
    for preset in sorted(presets):
        configure(layout, preset)
    for fault in selected:
        baseline, mutated, reason, verdict = run_fault(layout, fault, args.jobs)
        counts[verdict] += 1
        # The reason is scrubbed before the row cuts it short, since half a path matches
        # nothing, and the whole row again after, since every field reaches the file.
        lines.append(scrub(format_row(fault, baseline, mutated, scrub(reason), verdict)))
        print(lines[-1], flush=True)
    summary = " ".join(f"{verdict}={count}" for verdict, count in counts.items())
    lines.append(f"summary: {summary}")
    print(lines[-1], flush=True)
    leaks = [line for line in lines if LEAK.search(line)]
    if leaks:
        raise CannotRun(f"refusing to write output with a private path or address: {leaks[0]}")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text("\n".join(lines) + "\n")
    return 0 if counts["caught"] == len(selected) else 1


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", action="append", default=[], help="fault ids, comma-separated")
    parser.add_argument("--kind", choices=KINDS)
    parser.add_argument("--jobs", type=int, default=2, help=f"build jobs, at most {MAX_JOBS}")
    parser.add_argument("--source", type=Path, help="export to mutate, for a tree with no .git")
    parser.add_argument("--output", type=Path, help="also write the result lines here")
    parser.add_argument("--list", action="store_true", help="print the faults and stop")
    parser.add_argument("--manifest", type=Path, help="a manifest other than the committed one")
    parser.add_argument("--build-root", type=Path, help="default build/fault")
    parser.add_argument("--scope-note", help="why this run covers what it covers")
    args = parser.parse_args()
    if not 1 <= args.jobs <= MAX_JOBS:
        parser.error(f"--jobs must be between 1 and {MAX_JOBS}")
    if args.source:
        args.source = args.source.resolve()
        if not args.source.is_dir():
            parser.error(f"--source is not a directory: {args.source}")
    if args.build_root is None:
        args.build_root = default_build_root(args)
    args.build_root = args.build_root.resolve()
    return args


def main():
    args = parse_arguments()
    os.environ["AADP_JOBS"] = str(args.jobs)
    try:
        commit = source_commit(args)
        faults = parse_manifest(read_manifest_text(args, commit))
        selected = select_faults(faults, args)
        if args.list:
            for fault in selected:
                print(" | ".join((fault["id"], fault["kind"], fault["detect"], fault["covers"])))
            return 0
        layout = Layout(args.build_root, args.source.parent if args.source else REPO)
        layout.logs.mkdir(parents=True, exist_ok=True)
        with open(layout.root / ".lock", "w") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as error:
                raise CannotRun(f"another run holds {layout.root}") from error
            return run_all(faults, selected, layout, commit, args)
    except Exception as error:  # A crash here must not read as exit 1, a fault that got through.
        print(f"fault_injection: could not run: {error!r}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
