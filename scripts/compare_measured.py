#!/usr/bin/env python3
"""Rebuilds the rows of references/measured_results.csv from a plan run and compares bytes.

    python3 -I scripts/compare_measured.py <runner-output> <measured_results.csv>

The committed file is the runner's output put into CSV, under a comment header. Only the data
rows are compared. Exit codes: 0 identical, 1 different, 2 unusable input.
"""

import re
import sys

ROW = re.compile(
    r"^(?P<id>\S+)\s+estimate=(?P<estimate>\S+) reference=(?P<reference>\S+) "
    r"se=(?P<se>\S+) bound=(?P<bound>\S+) (?P<verdict>\S+) condition=\S+$"
)


def runner_rows(text):
    rows = []
    for line in text.splitlines():
        match = ROW.match(line)
        if match:
            rows.append(",".join(match[name] for name in match.groupdict()))
    return rows


def committed_rows(text):
    lines = [line for line in text.splitlines() if line and not line.startswith("#")]
    if not lines or not lines[0].startswith("id,"):
        return None
    return lines[1:]


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8") as file:
        produced = runner_rows(file.read())
    with open(argv[2], encoding="utf-8") as file:
        committed = committed_rows(file.read())
    if committed is None or not committed:
        print("compare_measured: no data rows in the committed file", file=sys.stderr)
        return 2
    if not produced:
        print("compare_measured: no result rows in the runner output", file=sys.stderr)
        return 2
    if produced == committed:
        print(f"compare_measured: {len(produced)} rows byte-identical")
        return 0
    print(f"compare_measured: DIFFERENT, {len(produced)} rows against {len(committed)}")
    for new, old in zip(produced, committed):
        if new != old:
            print(f"  first difference: produced {new}")
            print(f"                    committed {old}")
            break
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
