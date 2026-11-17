#!/usr/bin/env python3
"""QL-1's summary: paths/s and RMSE^2 x time, QuantLib against ours, from the reported files.

    python3 -I scripts/quantlib_summary.py --accuracy <curated csv> --speed <file> <file> <file>
        [--iteration]

--accuracy is the curated accuracy run (references/quantlib_accuracy.csv, written by
scripts/curate_quantlib_accuracy.py, which validates it here too). --speed is the three stamped
report files of `aadp_bench_quantlib speed --report` (table ql_speed): QuantLib's default column
with ours, its tuned column without LTO, and its tuned column with LTO, in any order. Prints
Markdown on stdout: the table, QL-1's verdict on its strictly like-for-like rows, and a
supplement with SE^2 alone, which has no verdict (docs/NOTES.md, "The QuantLib and Metal report
emitters").

RMSE^2 x time (docs/NOTES.md, "Stage 10"): RMSE^2 = max(SE^2, d^2) at the accuracy run's N, d =
estimate - reference, times the median time of one timed call scaled to that N. Ours, fastest, is
the faster of scalar and lanes, which give the same bits and so one accuracy row.

Refused: a speed file that is not a stamped report file with the ql_speed table, a column missing
or given twice, files from two hosts or two commits, an accuracy file from another host or
commit, and, without --iteration, a file that is not reportable or ran at a reduced size. With
--iteration everything is printed under an ITERATION ONLY banner.
Exit codes: 0 printed, 1 refused, 2 unusable arguments or files.
"""

import importlib.util
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent
ACCURACY_N = 4194304

QL_COLUMNS = ("ql-default", "ql-tuned-nolto", "ql-tuned")
SPEED_COLUMNS = [
    ("row", "text", "none"),
    ("column", "text", "none"),
    ("paths", "integer", "count"),
    ("steps", "integer", "count"),
    ("median", "real", "s"),
    ("min", "real", "s"),
    ("max", "real", "s"),
    ("paths_per_s", "real", "paths/s"),
    ("ns_per_path_step", "real", "ns"),
    ("repetitions", "integer", "count"),
]

# (label, speed row, our engine suffix, our accuracy row, QuantLib's accuracy row, class)
# Classes: strict (QL-1 judged on paths/s and RMSE^2 x time), speed (strict for speed only, no
# reference), none (no QuantLib column), bump (QuantLib's central bump against our tape), sanity.
ROWS = [
    ("R1", "R1", "", "R1/ours", "R1/quantlib", "strict"),
    ("R2.K100", "R2.K100", "", "R2.K100/ours", "R2.K100/quantlib", "strict"),
    ("R3", "R3", "", "R3/ours", "R3/quantlib", "strict"),
    ("R4", "R4", "", "R4/ours", "R4/quantlib", "strict"),
    ("R5.H095", "R5.H095", "", "R5.H095/ours", "R5.H095/quantlib", "strict"),
    ("R6.H095", "R6.H095", "", "R6.H095/ours", None, "none"),
    ("R7.H095", "R7.H095", "", "R7.H095/ours", "R7.H095/quantlib", "strict"),
    ("R9a", "R9a", "", "R9a/ours", "R9a/quantlib", "bump"),
    ("R9b (one-step)", "R9b", "-ours-one-step", "R9b/ours-one-step", "R9b/quantlib", "bump"),
    ("R9b (smoothed)", "R9b", "-ours-smoothed", "R9b/ours-smoothed", "R9b/quantlib", "bump"),
    ("R10", "R10", "", None, None, "speed"),
    ("R11", "R11", "", None, None, "speed"),
    ("R12", "R12", "", None, None, "sanity"),
]
CLASS_TEXT = {
    "strict": "yes",
    "speed": "speed only",
    "none": "no QuantLib",
    "bump": "no: bump against tape",
    "sanity": "no: sanity row",
}


class Refused(Exception):
    pass


def load_curation():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(
        "curate_quantlib_accuracy", ROOT / "curate_quantlib_accuracy.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_results_file(text, where):
    """The header and the report sections of a stamped results file."""
    lines = text.split("\n")
    if not lines or lines[0] != "aadp-results 1":
        raise Refused(f"{where}: not a results file")
    header = {}
    i = 1
    while i < len(lines) and lines[i] != "":
        key, sep, value = lines[i].partition(": ")
        if not sep:
            raise Refused(f"{where}: a header line without a value: {lines[i]!r}")
        header[key] = value
        i += 1
    sections = {}
    name = None
    for line in lines[i + 1 :]:
        if line.startswith("@section "):
            name = line[len("@section ") :]
            if name in sections:
                raise Refused(f"{where}: section {name} twice")
            sections[name] = []
        elif name is not None:
            sections[name].append(line)
    return header, sections


def parse_speed_table(lines, where):
    expected_head = ["type table"]
    if lines[:1] != expected_head:
        raise Refused(f"{where}: ql_speed is not a table")
    columns = [line.split(" ")[1:] for line in lines if line.startswith("column ")]
    if columns != [list(c) for c in SPEED_COLUMNS]:
        raise Refused(f"{where}: ql_speed's columns are not the bench's")
    if "timing yes" not in lines or "uses_chain no" not in lines:
        raise Refused(f"{where}: ql_speed is not a timed table off the chain")
    records = {}
    for line in lines:
        if not line.startswith("row "):
            continue
        fields = line.split(" ")[1:]
        if len(fields) != len(SPEED_COLUMNS) + 1:
            raise Refused(f"{where}: a row with {len(fields) - 1} cells")
        key, cells = fields[0], fields[1:]
        if key != f"{cells[0]}.{cells[1]}" or key in records:
            raise Refused(f"{where}: row key {key!r} repeated or not <row>.<column>")
        record = {"row": cells[0], "column": cells[1]}
        for (name, kind, _), cell in zip(SPEED_COLUMNS[2:], cells[2:]):
            value = int(cell) if kind == "integer" else float(cell)
            if not math.isfinite(value) or value <= 0:
                raise Refused(f"{where}: {key} {name} {cell}")
            record[name] = value
        records[key] = record
    if not records:
        raise Refused(f"{where}: ql_speed has no rows")
    return records


def read_speed(text, where, iteration):
    header, sections = parse_results_file(text, where)
    for key in ("reportable", "host_label", "commit", "ql_column", "report_tool"):
        if key not in header:
            raise Refused(f"{where}: the header has no {key}; is it stamped?")
    if "report_ql_speed" not in sections:
        raise Refused(f"{where}: no ql_speed table")
    if not iteration:
        if header["reportable"] != "yes":
            raise Refused(
                f"{where}: not reportable ({header.get('reportable_reason', 'no reason')}); "
                "pass --iteration to print it as iteration only"
            )
        if not header["report_tool"].endswith(" --scale 1"):
            raise Refused(f"{where}: run at a reduced size; pass --iteration")
    if header["ql_column"] not in QL_COLUMNS:
        raise Refused(f"{where}: QuantLib column {header['ql_column']!r}")
    return header, parse_speed_table(sections["report_ql_speed"], where)


def accuracy_rows(text, curation):
    rows = curation.validate(text, curation.registered_names())
    match = re.search(r"^# Commit ([0-9a-f]{40}), tree \w+, host ([a-z0-9-]+);", text, re.M)
    out = {}
    for row in rows:
        estimate, se, reference = float(row[1]), float(row[2]), float(row[3])
        out[row[0]] = {"se": se, "d": estimate - reference}
    return match[1], match[2], out


def mse(accuracy, name, se_only):
    a = accuracy[name]
    return a["se"] ** 2 if se_only else max(a["se"] ** 2, a["d"] ** 2)


def work(record, accuracy, name, se_only):
    """RMSE^2 (or SE^2) times the time of the accuracy run's N paths."""
    return mse(accuracy, name, se_only) * record["median"] * ACCURACY_N / record["paths"]


def summarise(accuracy_text, speed_texts, iteration=False):
    curation = load_curation()
    try:
        commit, host, accuracy = accuracy_rows(accuracy_text, curation)
    except curation.Refused as error:
        raise Refused(f"accuracy: {error}") from error
    merged = {}
    hosts, commits, columns = set(), set(), []
    for where, text in speed_texts:
        header, records = read_speed(text, where, iteration)
        hosts.add(header["host_label"])
        commits.add(header["commit"])
        columns.append(header["ql_column"])
        for key, record in records.items():
            if key in merged:
                raise Refused(f"{where}: {key} is in two files")
            merged[key] = record
    if sorted(columns) != sorted(QL_COLUMNS):
        raise Refused(f"the speed files' QuantLib columns are {columns}, not one of each")
    if len(hosts) != 1 or len(commits) != 1:
        raise Refused(f"the speed files come from hosts {sorted(hosts)} and commits {sorted(commits)}")
    if not iteration and (hosts != {host} or commits != {commit}):
        raise Refused(f"the accuracy run is {host} at {commit}, the speed runs are not")
    out = []
    for label, row, suffix, ours_name, ql_name, cls in ROWS:
        scalar = merged.get(f"{row}.ours-scalar{suffix}")
        lanes = merged.get(f"{row}.ours-lanes{suffix}")
        if scalar is None:
            raise Refused(f"no ours-scalar{suffix} timing for {row}")
        fastest = lanes if lanes and lanes["paths_per_s"] > scalar["paths_per_s"] else scalar
        entry = {"label": label, "class": cls, "scalar": scalar, "lanes": lanes,
                 "fastest": fastest, "ql": {}}  # fmt: skip
        for column in QL_COLUMNS:
            record = merged.get(f"{row}.{column}")
            if (record is None) != (cls == "none"):
                raise Refused(f"{row}: QuantLib column {column} {'missing' if record is None else 'unexpected'}")
            if record is None:
                continue
            q = {"record": record,
                 "per_scalar": record["paths_per_s"] / scalar["paths_per_s"],
                 "per_fastest": record["paths_per_s"] / fastest["paths_per_s"]}  # fmt: skip
            if ql_name:
                for se_only, key in ((False, "work"), (True, "se_work")):
                    q[key] = work(record, accuracy, ql_name, se_only)
                    q[key + "_over_fastest"] = q[key] / work(fastest, accuracy, ours_name, se_only)
                    q[key + "_over_scalar"] = q[key] / work(scalar, accuracy, ours_name, se_only)
            entry["ql"][column] = q
        if ours_name:
            for se_only, key in ((False, "work"), (True, "se_work")):
                entry[key + "_fastest"] = work(fastest, accuracy, ours_name, se_only)
                entry[key + "_scalar"] = work(scalar, accuracy, ours_name, se_only)
        if cls in ("strict", "speed"):
            tuned = entry["ql"]["ql-tuned"]
            verdict = {}
            for against in ("scalar", "fastest"):
                faster = tuned[f"per_{against}"] < 1.0
                better = cls == "speed" or tuned[f"work_over_{against}"] > 1.0
                verdict[against] = faster and better
            entry["verdict"] = verdict
        out.append(entry)
    return {"commit": commit, "host": host, "iteration": iteration, "rows": out}


def g(x, digits=4):
    return f"{x:.{digits}g}"


def render(summary):
    lines = []
    if summary["iteration"]:
        lines += ["**ITERATION ONLY: not results.**", ""]
    lines += [
        f"QL-1 on `{summary['host']}` at {summary['commit'][:7]}: one thread on both sides. "
        "Paths/s in millions. RMSE² × time is max(SE², d²) at the accuracy run's "
        f"{ACCURACY_N} paths times the median time scaled to them, in s. Ours, fastest, is the "
        "faster of scalar and lanes. QL-1 is judged on the strictly like-for-like rows against "
        "QuantLib's tuned column: met when it is slower than ours in paths/s and has the larger "
        "RMSE² × time (speed-only rows: paths/s alone).",
        "",
        "| Row | like-for-like | ours scalar | ours lanes | QL default | QL tuned, no LTO | QL "
        "tuned | default / scalar | default / fastest | tuned / scalar | tuned / fastest | "
        "RMSE² × time: ours fastest | QL default | QL tuned, no LTO | QL tuned | default / ours "
        "fastest | tuned / ours fastest | QL-1, against scalar | QL-1, against fastest |",
        "|" + "---|" * 19,
    ]
    met = {"scalar": [0, 0], "fastest": [0, 0]}
    for e in summary["rows"]:
        ql = e["ql"]
        speed = [g(e["scalar"]["paths_per_s"] / 1e6), g(e["lanes"]["paths_per_s"] / 1e6)
                 if e["lanes"] else "none"]  # fmt: skip
        speed += [g(ql[c]["record"]["paths_per_s"] / 1e6) if c in ql else "none" for c in QL_COLUMNS]
        ratios = []
        for c in ("ql-default", "ql-tuned"):
            for against in ("scalar", "fastest"):
                ratios.append(g(ql[c][f"per_{against}"], 3) if c in ql else "")
        if "work_fastest" in e:
            works = [g(e["work_fastest"])]
            works += [g(ql[c]["work"]) if c in ql and "work" in ql[c] else "none" for c in QL_COLUMNS]
            works += [g(ql[c]["work_over_fastest"], 3) if c in ql and "work" in ql[c] else ""
                      for c in ("ql-default", "ql-tuned")]  # fmt: skip
        else:
            works = ["none (no reference)", "", "", "", "", ""]
        verdicts = ["", ""]
        if "verdict" in e:
            verdicts = []
            for against in ("scalar", "fastest"):
                ok = e["verdict"][against]
                met[against][0] += 1 if ok else 0
                met[against][1] += 1
                verdicts.append("met" if ok else "NOT MET")
        lines.append("| " + " | ".join([e["label"], CLASS_TEXT[e["class"]]] + speed + ratios
                                       + works + verdicts) + " |")  # fmt: skip
    lines.append("")
    for against in ("scalar", "fastest"):
        n_met, n = met[against]
        word = "met" if n_met == n else "NOT MET"
        lines.append(
            f"QL-1 against ours, {against}: {word}, on {n_met} of {n} strictly like-for-like rows."
        )
    lines += [
        "",
        "Supplement, no verdict: RMSE² × time with SE² alone, which leaves out the distance d. "
        "QuantLib's batches share the seeds 100 × base + 1 + b on every row but R5, so its rows' "
        "distances are correlated (docs/NOTES.md, \"Stage 10 results\"); this shows the ratios "
        "without them.",
        "",
        "| Row | SE² × time: ours fastest | QL default | QL tuned, no LTO | QL tuned | default / "
        "ours fastest | tuned / ours fastest |",
        "|" + "---|" * 7,
    ]
    for e in summary["rows"]:
        if "se_work_fastest" not in e or not e["ql"]:
            continue
        ql = e["ql"]
        cells = [g(e["se_work_fastest"])] + [g(ql[c]["se_work"]) for c in QL_COLUMNS]
        cells += [g(ql[c]["se_work_over_fastest"], 3) for c in ("ql-default", "ql-tuned")]
        lines.append("| " + " | ".join([e["label"]] + cells) + " |")
    return "\n".join(lines) + "\n"


def main(argv):
    args = argv[1:]
    iteration = "--iteration" in args
    args = [a for a in args if a != "--iteration"]
    if len(args) != 6 or args[0] != "--accuracy" or args[2] != "--speed":
        print(__doc__, file=sys.stderr)
        return 2
    try:
        accuracy_text = pathlib.Path(args[1]).read_text(encoding="utf-8")
        speed = [(p, pathlib.Path(p).read_text(encoding="utf-8")) for p in args[3:]]
    except OSError as error:
        print(f"quantlib_summary: {error}", file=sys.stderr)
        return 2
    try:
        print(render(summarise(accuracy_text, speed, iteration)), end="")
    except Refused as error:
        print(f"quantlib_summary: refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
