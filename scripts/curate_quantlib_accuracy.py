#!/usr/bin/env python3
"""Curates the QuantLib accuracy run (QL-2) into references/, and checks a curated file.

    python3 -I scripts/curate_quantlib_accuracy.py curate <raw output> <curated csv>
    python3 -I scripts/curate_quantlib_accuracy.py check <raw output> <curated csv>
    python3 -I scripts/curate_quantlib_accuracy.py validate <curated csv>
    python3 -I scripts/curate_quantlib_accuracy.py validate-if-present <curated csv>

The raw output is what `aadp_bench_quantlib accuracy --ceilings bench/quantlib/ceilings.csv
--out <file>` writes, run through heavy.sh as registered (docs/NOTES.md, "Stage 10", and "The
QuantLib and Metal report emitters"). The curated file is its 42 accuracy rows as CSV under a
comment header, on the precedent of references/measured_results_run2.csv.

curate refuses a raw output that is not the registered run: another N, seed, B, K, critical value
or ceilings file, a tree that is neither clean nor exported, a missing, repeated, extra or
misordered comparison, an error line, a tally that does not match the rows, a run that did not
end, or anything naming the option chain's data, from which no QuantLib row is derived. check
rebuilds the rows from the raw output and compares them byte for byte with the curated file's.
validate reads a curated file alone: the registered header line, the 42 comparisons in the order
of bench/quantlib/ceilings.csv, and each row's bound, z and verdict recomputed from its own cells
by the section 10 rule. validate-if-present does the same, and passes with a note when the file
does not exist yet.

Exit codes: 0 done or valid, 1 refused, different or invalid, 2 unusable arguments or files.
"""

import csv
import io
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CEILINGS = "bench/quantlib/ceilings.csv"

# The registered run (docs/NOTES.md, "Stage 10"): N = 2^22 per comparison on seed 20261112,
# B = 64 batches, K = 42 comparisons, critical value t(1 - 0.01 / 84, 63).
CRITICAL = 3.897574976
REGISTERED = f"# critical {CRITICAL}, K 42, B 64, N 4194304, seed 20261112"
REGISTERED_CURATED = (
    f"# N 4194304 per comparison, seed 20261112, B 64, K 42, critical {CRITICAL}; ceilings "
    f"{CEILINGS}."
)
COLUMNS = [
    "name",
    "estimate",
    "se",
    "reference",
    "b",
    "ceiling",
    "bound",
    "z",
    "verdict",
    "check",
    "reference_source",
]
VERDICTS = ("pass", "FAIL distance", "FAIL se-ceiling", "FAIL both")
# Nothing of the option chain enters a QuantLib row; a raw output that names its data is not the
# registered run.
CHAIN_WORDS = re.compile(r"TAIFEX|data\.gov\.tw|DailyMarketReport|data/")


class Refused(Exception):
    pass


def registered_names():
    """The 42 comparisons in report order, from the committed ceilings file."""
    text = (ROOT / CEILINGS).read_text(encoding="utf-8")
    rows = [line for line in text.splitlines() if line and not line.startswith("#")]
    if not rows or not rows[0].startswith("name,"):
        raise Refused(f"{CEILINGS} has no header row")
    names = [row.split(",", 1)[0] for row in rows[1:]]
    if len(names) != 42 or len(set(names)) != 42:
        raise Refused(f"{CEILINGS} does not name 42 comparisons")
    return names


def header_value(lines, prefix):
    found = [line[len(prefix) :] for line in lines if line.startswith(prefix)]
    if len(found) != 1:
        raise Refused(f"the raw output has {len(found)} lines starting {prefix.strip()!r}")
    return found[0]


def parse_raw(text, names):
    """The raw output's parts, or Refused with the reason."""
    if CHAIN_WORDS.search(text):
        raise Refused("the raw output names the option chain's data; no QuantLib row uses it")
    lines = text.splitlines()
    if not lines or not lines[0].startswith("# aadp_bench_quantlib accuracy "):
        raise Refused("not the output of aadp_bench_quantlib accuracy")
    commit = header_value(lines, "# commit: ")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise Refused(f"no full commit hash: {commit!r}")
    tree = header_value(lines, "# tree: ")
    if not (tree == "clean (git)" or tree == "exported (COMMIT file)"):
        raise Refused(f"the tree was {tree!r}, neither clean nor exported")
    if header_value(lines, "# ceilings: ") != CEILINGS:
        raise Refused(f"the ceilings are not {CEILINGS}")
    if REGISTERED not in lines:
        raise Refused(f"not the registered run: no line {REGISTERED!r}")
    errors = [line for line in lines if line.startswith("error,")]
    if errors:
        raise Refused(f"the run has an error line: {errors[0]}")
    columns = "accuracy," + ",".join(COLUMNS)
    if lines.count(columns) != 1:
        raise Refused("the accuracy column line is missing or repeated")
    rows = []
    for line in lines:
        if line.startswith("accuracy,") and line != columns:
            fields = line.split(",", len(COLUMNS))
            if len(fields) != len(COLUMNS) + 1:
                raise Refused(f"a short accuracy row: {line}")
            rows.append(fields[1:])
    got = [row[0] for row in rows]
    if got != names:
        missing = [n for n in names if n not in got]
        extra = [n for n in got if n not in names]
        raise Refused(
            f"the comparisons are not the registered 42 in order: {len(got)} rows, "
            f"missing {missing[:3]}, extra or repeated {extra[:3] or sorted_repeats(got)[:3]}"
        )
    for row in rows:
        if row[8] not in VERDICTS:
            raise Refused(f"{row[0]}: verdict {row[8]!r}")
    tally = re.compile(r"^# comparisons failed (\d+), checks failed (\d+)$")
    tallies = [tally.match(line) for line in lines if tally.match(line)]
    ended = bool(tally.match(lines[-1])) or (
        lines[-1] == "# INVALID: a check failed" and len(lines) > 1 and tally.match(lines[-2])
    )
    if len(tallies) != 1 or not ended:
        raise Refused("the run did not end with its tally")
    failed = sum(1 for row in rows if row[8] != "pass")
    if int(tallies[0][1]) != failed:
        raise Refused(f"the tally says {tallies[0][1]} failed, the rows {failed}")
    checks = [line for line in lines if line.startswith("check,")]
    sanity = [line for line in lines if line.startswith("sanity,")]
    if len(checks) != 1 or len(sanity) != 3:
        raise Refused("the grid check or the three sanity lines are missing")
    record = {}
    for key in ("QL_VERSION", "QL_HEX_VERSION", "BOOST_VERSION"):
        record[key] = header_value(lines, f"# {key}: ")
    return {
        "commit": commit,
        "tree": tree.split(" ", 1)[0],
        "host": header_value(lines, "# host: "),
        "compiler": header_value(lines, "# compiler: "),
        "flags": header_value(lines, "# flags: "),
        "link": header_value(lines, "# link: "),
        "record": record,
        "r9": [line for line in lines if line.startswith("# R9a reference") or line.startswith(
            "# R9b reference")],
        "tally": tallies[0][0][2:],
        "checks": checks,
        "sanity": sanity,
        "rows": rows,
    }


def sorted_repeats(names):
    return sorted({n for n in names if names.count(n) > 1})


def data_rows(rows):
    out = io.StringIO()
    writer = csv.writer(out, lineterminator="\n")
    writer.writerow(COLUMNS)
    for row in rows:
        writer.writerow(row)
    return out.getvalue()


def render(raw):
    record = raw["record"]
    header = [
        '# QuantLib comparison, accuracy (QL-2): aadp_bench_quantlib accuracy, curated by '
        "scripts/curate_quantlib_accuracy.py (docs/NOTES.md, \"The QuantLib and Metal report "
        'emitters").',
        f"# Commit {raw['commit']}, tree {raw['tree']}, host {raw['host']}; QuantLib "
        f"{record['QL_VERSION']} ({record['QL_HEX_VERSION']}), Boost {record['BOOST_VERSION']}; "
        f"{raw['compiler']}; {raw['flags']}; {raw['link']}.",
        REGISTERED_CURATED,
        "# bound = b + critical * se; z = (estimate - reference) / se; pass when |estimate - "
        "reference| <= bound and se <= ceiling (BENCHMARKS.md section 10).",
    ]
    header += ["# " + line[2:] for line in raw["r9"]]
    header.append(f"# {raw['tally']}.")
    header += ["# " + line for line in raw["checks"] + raw["sanity"]]
    header += [
        "# No row is derived from the option chain: R10 to R12 price on a synthetic surface "
        "written in the bench.",
        "# QuantLib's batches share the seeds 100 x base + 1 + b on every row but R5, so its "
        "rows' errors are correlated (docs/NOTES.md, \"Stage 10 results\").",
    ]
    return "\n".join(header) + "\n" + data_rows(raw["rows"])


def curated_rows(text):
    lines = [line for line in text.splitlines(keepends=True) if not line.startswith("#")]
    return "".join(lines)


def number(row, index):
    try:
        value = float(row[index])
    except ValueError as error:
        raise Refused(f"{row[0]}: {COLUMNS[index]} {row[index]!r} is not a number") from error
    if not math.isfinite(value):
        raise Refused(f"{row[0]}: {COLUMNS[index]} is not finite")
    return value


def close(a, b, tolerance):
    return abs(a - b) <= tolerance


def judge_row(row):
    """Recomputes a curated row's bound, z and verdict from its cells, by the section 10 rule.

    The cells are printed to 10 significant digits, so each recomputation allows that rounding:
    a value within it of a boundary may carry either verdict."""
    estimate, se, reference, b, ceiling, bound, z = (number(row, i) for i in range(1, 8))
    if se <= 0:
        raise Refused(f"{row[0]}: se is not positive")
    digit = 1e-9  # Half a unit in the 10th significant digit, relative, with room.
    recomputed = b + CRITICAL * se
    if not close(bound, recomputed, digit * (abs(b) + CRITICAL * se) + 1e-15):
        raise Refused(f"{row[0]}: bound {bound} is not b + critical * se = {recomputed}")
    distance = estimate - reference
    slack = digit * (abs(estimate) + abs(reference))
    if not close(z, distance / se, slack / se + digit * abs(z) + 1e-12):
        raise Refused(f"{row[0]}: z {z} is not (estimate - reference) / se = {distance / se}")
    near_bound = abs(abs(distance) - bound) <= slack + digit * bound
    near_ceiling = abs(se - ceiling) <= digit * ceiling
    within_bound = abs(distance) <= bound
    within_ceiling = se <= ceiling
    if within_bound and within_ceiling:
        expected = {"pass"}
    elif not within_bound and not within_ceiling:
        expected = {"FAIL both"}
    else:
        expected = {"FAIL se-ceiling" if within_bound else "FAIL distance"}
    if near_bound or near_ceiling:
        expected = set(VERDICTS)
    if row[8] not in expected:
        raise Refused(f"{row[0]}: verdict {row[8]!r}, but the row gives {sorted(expected)}")


def validate(text, names):
    lines = text.splitlines()
    if REGISTERED_CURATED not in lines:
        raise Refused(f"no line {REGISTERED_CURATED!r}")
    if not re.search(r"^# Commit [0-9a-f]{40}, tree (clean|exported), host ", text, re.M):
        raise Refused("no commit line with a clean or exported tree")
    if CHAIN_WORDS.search(text):
        raise Refused("the file names the option chain's data; no QuantLib row uses it")
    rows = list(csv.reader(io.StringIO(curated_rows(text))))
    if not rows or rows[0] != COLUMNS:
        raise Refused("the column row is missing or not " + ",".join(COLUMNS))
    rows = rows[1:]
    if [row[0] if row else "" for row in rows] != names:
        raise Refused(f"the comparisons are not the registered 42 in order ({len(rows)} rows)")
    for row in rows:
        if len(row) != len(COLUMNS):
            raise Refused(f"{row[0]}: {len(row)} cells")
        if row[8] not in VERDICTS:
            raise Refused(f"{row[0]}: verdict {row[8]!r}")
        judge_row(row)
    tally = re.search(r"^# comparisons failed (\d+), checks failed (\d+)\.$", text, re.M)
    if not tally or int(tally[1]) != sum(1 for row in rows if row[8] != "pass"):
        raise Refused("the tally line is missing or does not match the rows")
    return rows


def read(path):
    try:
        return pathlib.Path(path).read_text(encoding="utf-8")
    except OSError as error:
        print(f"curate_quantlib_accuracy: cannot read {path}: {error}", file=sys.stderr)
        return None


def main(argv):
    command = argv[1] if len(argv) > 1 else ""
    shapes = {"curate": 4, "check": 4, "validate": 3, "validate-if-present": 3}
    if shapes.get(command) != len(argv):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        names = registered_names()
        if command == "validate-if-present" and not pathlib.Path(argv[2]).exists():
            print(f"curate_quantlib_accuracy: {argv[2]} is absent: nothing curated yet")
            return 0
        if command in ("validate", "validate-if-present"):
            text = read(argv[2])
            if text is None:
                return 2
            rows = validate(text, names)
            print(f"curate_quantlib_accuracy: {argv[2]} valid, {len(rows)} rows")
            return 0
        raw_text = read(argv[2])
        if raw_text is None:
            return 2
        raw = parse_raw(raw_text, names)
        if command == "curate":
            output = render(raw)
            validate(output, names)
            pathlib.Path(argv[3]).write_text(output, encoding="utf-8")
            print(f"curate_quantlib_accuracy: wrote {len(raw['rows'])} rows to {argv[3]}")
            return 0
        text = read(argv[3])
        if text is None:
            return 2
        produced = data_rows(raw["rows"])
        committed = curated_rows(text)
        if produced == committed:
            print(f"curate_quantlib_accuracy: {len(raw['rows'])} rows byte-identical")
            return 0
        print("curate_quantlib_accuracy: DIFFERENT rows")
        for new, old in zip(produced.splitlines(), committed.splitlines()):
            if new != old:
                print(f"  first difference: produced {new}")
                print(f"                    curated  {old}")
                break
        return 1
    except Refused as error:
        print(f"curate_quantlib_accuracy: refused: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
