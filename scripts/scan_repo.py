#!/usr/bin/env python3
"""Fails if a tracked file holds something private or a line of the raw market data.

    python3 -I scripts/scan_repo.py [--root <dir>]

Reads the working-tree bytes of every file git tracks, binary files included, and the tracked
paths themselves. Standard library only. Exit codes: 0 clean, 1 findings, 2 could not run.

Kinds of finding:
  home-path     an absolute path into a home directory
  ipv4          an IPv4 address
  user, host    the current user and host names, read at run time, in any case
  private-note  an agent notes file, or a marker of one in a file's text
  taifex-row    a whole data row of a file in data/, spaces ignored, in cp950 or UTF-8
  tracked-data  a file under data/ that git tracks

Exceptions, each counted in the output so that none is silent:
  - Home path: a path component has to follow, so prose naming the directories is not a hit.
    A path inside a URL is not a hit either. SYNTHETIC_HOME_PATHS lists exact file and text
    pairs.
  - IPv4, by value: the three RFC 5737 documentation ranges, 127.0.0.0/8 and 0.0.0.0.
  - IPv4, by context: text right after "section", "version" or "v" reads as a numbering, as
    in a handbook section number. Never for 10/8, 100.64/10, 169.254/16, 172.16/12 or
    192.168/16: an address there always fails, whatever precedes it.
  - IPv4, by file: SYNTHETIC_ADDRESSES lists exact file and address pairs.
  - A dotted run of five or more parts, or an octet above 255, is not an address.
  - User and host: a name that identifies nobody, such as the CI account or a host named after
    its distribution, or a name under three characters, is not searched for, and the output
    says so. A scan on such a machine is weaker than one on a personal machine.
  - Private note: the one .gitignore line that keeps the local settings file out.
  - A mention of the research folder is not a note: docs and scripts name it on purpose.
  - TAIFEX: the first line of each file, the column header, is not a data row. A row with a
    changed field is not caught, only a whole one. With no data/ this check cannot run, and
    the output says so.
"""

import argparse
import os
import re
import socket
import subprocess
import sys

try:
    import pwd
except ImportError:  # Not on Windows.
    pwd = None

# The lookbehind leaves out a URL's path and a relative path, and keeps file:///.
HOME_PATH = re.compile(
    rb"(?<![A-Za-z0-9_.~-])/(?:Users|home|root)/[A-Za-z0-9._-]+"
    rb"|(?<![A-Za-z0-9])[A-Za-z]:[\\/]+Users[\\/]+[A-Za-z0-9._-]+"
)

IPV4 = re.compile(rb"(?<![\d.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?!\d|\.\d)")
NUMBERING_CONTEXT = re.compile(rb"(?:section|version|\bv)[\s:=]*$", re.IGNORECASE)
DOCUMENTATION_RANGES = ((192, 0, 2), (198, 51, 100), (203, 0, 113))

# A test of the privacy filter has to feed it these. Made up, and exact to the file. Literals
# here and in ALLOWED_NOTE_LINE are split so that this file does not match itself.
SYNTHETIC_ADDRESSES = {("tests/bench/privacy_test.cpp", (10, 1, 2, 3))}
SYNTHETIC_HOME_PATHS = {("tests/bench/privacy_test.cpp", b"C:" + rb"\\Users\\bob")}

GENERIC_USERS = {"root", "runner", "admin", "administrator", "ubuntu", "debian", "user", "ci"}
GENERIC_HOSTS = {"localhost", "debian", "ubuntu", "fedora", "archlinux", "raspberrypi"}

NOTE_FILES = re.compile(
    r"(?:^|/)(?:CLAUDE(?:\.local)?\.md|AGENTS\.md|GEMINI\.md|\.cursorrules|\.windsurfrules"
    r"|copilot-instructions\.md)$"
    r"|(?:^|/)\.(?:claude|cursor|orchestrator|research)/",
    re.IGNORECASE,
)
# The bracket keeps this line from matching itself.
NOTE_MARKERS = re.compile(
    rb"CLAUDE[.]md|[.]claude/|claude[-]opus-|Co-Authored-By[:]|Generated with \[?Claude",
    re.IGNORECASE,
)
ALLOWED_NOTE_LINE = (".gitignore", b"/." + b"claude/settings.local.json")

KINDS = (
    "home-path",
    "ipv4",
    "user",
    "host",
    "private-note",
    "taifex-row",
    "tracked-data",
)


class CannotRun(Exception):
    pass


def tracked_paths(root):
    try:
        result = subprocess.run(
            ["git", "-C", root, "ls-files", "-z"], capture_output=True, check=False
        )
    except OSError as error:
        raise CannotRun(f"cannot run git: {error}") from error
    if result.returncode != 0:
        raise CannotRun("git ls-files failed: " + result.stderr.decode(errors="replace").strip())
    paths = [path for path in result.stdout.decode("utf-8", "surrogateescape").split("\0") if path]
    if not paths:
        raise CannotRun("git lists no tracked files")
    return paths


def current_user():
    for variable in ("USER", "LOGNAME"):
        name = os.environ.get(variable, "").strip()
        if name:
            return name
    if pwd is not None:
        try:
            return pwd.getpwuid(os.getuid()).pw_name
        except KeyError:
            pass
    raise CannotRun("cannot read the current user name")


def current_host_forms():
    host = socket.gethostname().strip()
    if not host:
        raise CannotRun("cannot read the host name")
    forms = {host.lower(), host.split(".")[0].lower()}
    return sorted(form for form in forms if form)


def name_pattern(names):
    alternatives = b"|".join(re.escape(name.encode()) for name in names)
    return re.compile(alternatives, re.IGNORECASE)


def searchable(name, generic):
    return len(name) >= 3 and name.lower() not in generic


class RawRows:
    """Data rows of data/, indexed by a prefix. A row counts wherever it sits in a line."""

    def __init__(self):
        self.forms = []
        self.first_fields = set()
        self.count = 0
        self.prefix_length = 0
        self.by_prefix = {}

    @staticmethod
    def compact(line):
        return line.translate(None, b" \t\r\n")

    def add(self, row):
        compact = self.compact(row)
        if not compact:
            return
        self.count += 1
        self.first_fields.add(compact.split(b",", 1)[0])
        self.forms.append(compact)
        try:
            self.forms.append(compact.decode("cp950").encode("utf-8"))
        except UnicodeDecodeError:
            pass

    def seal(self):
        self.prefix_length = min(len(form) for form in self.forms)
        for form in self.forms:
            self.by_prefix.setdefault(form[: self.prefix_length], set()).add(form)

    def holds(self, line):
        compact = self.compact(line)
        for field in self.first_fields:
            start = compact.find(field)
            while start >= 0:
                key = compact[start : start + self.prefix_length]
                for row in self.by_prefix.get(key, ()):
                    if compact.startswith(row, start):
                        return True
                start = compact.find(field, start + 1)
        return False


def load_raw_rows(root):
    data_dir = os.path.join(root, "data")
    if not os.path.isdir(data_dir):
        return None, 0
    rows = RawRows()
    files = 0
    try:
        for name in sorted(os.listdir(data_dir)):
            path = os.path.join(data_dir, name)
            if name.startswith(".") or not os.path.isfile(path):
                continue
            with open(path, "rb") as file:
                lines = file.read().splitlines()
            files += 1
            for line in lines[1:]:
                rows.add(line)
    except OSError as error:
        raise CannotRun(f"cannot read data/: {error}") from error
    if rows.count == 0:
        raise CannotRun("data/ is present but holds no data rows")
    rows.seal()
    return rows, files


def line_of(content, offset):
    return content.count(b"\n", 0, offset) + 1


def read_tracked(root, path):
    full = os.path.join(root, path)
    if os.path.islink(full):
        return os.fsencode(os.readlink(full))
    try:
        with open(full, "rb") as file:
            return file.read()
    except FileNotFoundError:
        return None
    except OSError as error:
        raise CannotRun(f"cannot read a tracked file: {path}: {error}") from error


def ipv4_verdict(path, octets, before):
    """Returns None for a true hit, or the name of the exception that covers it."""
    if max(octets) > 255:
        return "not-an-address"
    if octets[:3] in DOCUMENTATION_RANGES or octets[0] == 127 or not any(octets):
        return "ipv4-documentation"
    if (path, octets) in SYNTHETIC_ADDRESSES:
        return "ipv4-synthetic"
    first, second = octets[:2]
    always_private = (
        first == 10
        or (first == 100 and 64 <= second < 128)
        or (first == 169 and second == 254)
        or (first == 172 and 16 <= second < 32)
        or (first == 192 and second == 168)
    )
    if not always_private and NUMBERING_CONTEXT.search(before):
        return "ipv4-numbering"
    return None


class Scan:
    def __init__(self, user_pattern, host_pattern, rows):
        self.user_pattern = user_pattern
        self.host_pattern = host_pattern
        self.rows = rows
        self.findings = []
        self.excepted = {}

    def hit(self, kind, path, line):
        self.findings.append((path, line, kind))

    def excuse(self, reason):
        self.excepted[reason] = self.excepted.get(reason, 0) + 1

    def scan_text(self, path, content, line_for):
        for match in HOME_PATH.finditer(content):
            if (path, match.group()) in SYNTHETIC_HOME_PATHS:
                self.excuse("home-path-synthetic")
            else:
                self.hit("home-path", path, line_for(match.start()))
        for match in IPV4.finditer(content):
            octets = tuple(int(group) for group in match.groups())
            before = content[max(0, match.start() - 16) : match.start()]
            reason = ipv4_verdict(path, octets, before)
            if reason is None:
                self.hit("ipv4", path, line_for(match.start()))
            else:
                self.excuse(reason)
        if self.user_pattern is not None:
            for match in self.user_pattern.finditer(content):
                self.hit("user", path, line_for(match.start()))
        if self.host_pattern is not None:
            for match in self.host_pattern.finditer(content):
                self.hit("host", path, line_for(match.start()))

    def scan_notes(self, path, content):
        for match in NOTE_MARKERS.finditer(content):
            line_number = line_of(content, match.start())
            line = content.split(b"\n")[line_number - 1].strip()
            if (path, line) == ALLOWED_NOTE_LINE:
                self.excuse("note-gitignore-line")
            else:
                self.hit("private-note", path, line_number)

    def scan_file(self, path, content):
        self.scan_text(path, content, lambda offset: line_of(content, offset))
        self.scan_notes(path, content)
        if self.rows is not None:
            for number, line in enumerate(content.split(b"\n"), start=1):
                if self.rows.holds(line):
                    self.hit("taifex-row", path, number)

    def scan_path(self, path):
        # Line 0 says the path itself matched.
        self.scan_text(path, os.fsencode(path), lambda offset: 0)
        if NOTE_FILES.search(path):
            self.hit("private-note", path, 0)
        if path == "data" or path.startswith("data/"):
            self.hit("tracked-data", path, 0)


def run(root):
    paths = tracked_paths(root)

    user = current_user()
    user_pattern = name_pattern([user]) if searchable(user, GENERIC_USERS) else None
    hosts = [host for host in current_host_forms() if searchable(host, GENERIC_HOSTS)]
    host_pattern = name_pattern(hosts) if hosts else None
    rows, data_files = load_raw_rows(root)

    scan = Scan(user_pattern, host_pattern, rows)
    missing = 0
    for path in paths:
        scan.scan_path(path)
        content = read_tracked(root, path)
        if content is None:
            missing += 1
            continue
        scan.scan_file(path, content)

    print(f"scan_repo: {len(paths)} tracked files, {missing} missing from the working tree")
    print(
        "scan_repo: user name "
        + ("searched" if user_pattern is not None else "NOT searched, generic or too short")
        + ", host name "
        + ("searched" if host_pattern is not None else "NOT searched, generic or too short")
    )
    if rows is None:
        print("scan_repo: TAIFEX row check did NOT run: there is no data/ directory")
    else:
        print(f"scan_repo: TAIFEX row check ran on {rows.count} rows from {data_files} files")
    counts = {kind: 0 for kind in KINDS}
    for _, _, kind in scan.findings:
        counts[kind] += 1
    print("scan_repo: findings " + " ".join(f"{kind}={counts[kind]}" for kind in KINDS))
    if scan.excepted:
        excepted = " ".join(f"{name}={scan.excepted[name]}" for name in sorted(scan.excepted))
        print("scan_repo: excepted " + excepted)
    # Kind and place only: a finding is never echoed, so no log repeats the private text.
    for path, line, kind in sorted(set(scan.findings)):
        print(f"{path}:{line}: {kind}")
    if scan.findings:
        print(f"scan_repo: FAIL, {len(scan.findings)} findings")
        return 1
    print("scan_repo: clean")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Scans tracked files for private content.")
    default_root = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
    parser.add_argument("--root", default=default_root, help="repository root")
    arguments = parser.parse_args()
    try:
        return run(arguments.root)
    except CannotRun as error:
        print(f"scan_repo: could not run: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
