#!/usr/bin/env python3
"""QL-1's summary (scripts/quantlib_summary.py) on hand-worked inputs.

    python3 -I tests/bench/quantlib_summary_test.py

A curated accuracy file and three stamped speed files, written here with made-up numbers chosen
so that every figure can be worked by hand, must give the paths/s ratios, RMSE^2 x time and its
ratios, the SE^2-only supplement and QL-1's verdicts below; and every mixture the summary must
refuse is refused. Exit 0 if all hold.
"""

import importlib.util
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CRITICAL = 3.897574976
COMMIT = "0123456789abcdef0123456789abcdef01234567"
SE = 1.0 / 1024.0


def load(name, relative):
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def g10(x):
    return f"{x:.10g}"


# Every comparison passes. Each reference is 1 and each SE is 1/1024 unless set below, so the
# numbers are exact in binary. R1/ours sits 0.5 SE from its reference, so its RMSE^2 is SE^2 =
# 2^-20; R1/quantlib sits 2 SE (its SE 1/512) away, so its RMSE^2 is d^2 = 4 (2^-9)^2 = 2^-16 and
# its SE^2 2^-18. Every other row sits 0.5 SE away.
SPECIAL = {"R1/quantlib": (2.0 / 512.0, 1.0 / 512.0)}  # name: (d, se)


def accuracy_text(curation, commit=COMMIT, host="mac-m4max"):
    rows = []
    for name in curation.registered_names():
        d, se = SPECIAL.get(name, (0.5 * SE, SE))
        estimate = 1.0 + d
        rows.append([name, g10(estimate), g10(se), "1", "0", "0.01", g10(CRITICAL * se),
                     g10(d / se), "pass", "lanes-equal-scalar bits", "made up"])  # fmt: skip
    header = [
        "# QuantLib comparison, accuracy (QL-2): made up for this test.",
        f"# Commit {commit}, tree exported, host {host}; made up.",
        curation.REGISTERED_CURATED,
        "# comparisons failed 0, checks failed 0.",
    ]
    return "\n".join(header) + "\n" + curation.data_rows(rows)


# Timings, in s at 2^20 paths (2^18 on R10 to R12): R1 ours scalar 0.25, lanes 0.2, QuantLib
# default 1.0, tuned without LTO 0.9, tuned 0.8. R3 has no lanes: ours 0.5 against tuned 0.4, so
# there QuantLib is faster and QL-1 is not met. Every other row: ours 0.5 and 0.4 on lanes where it
# has them, QuantLib 2.0, 1.9 and 1.8.
def timings():
    ours = {}
    ql = {}
    for row in ("R1", "R2.K100", "R3", "R4", "R5.H095", "R6.H095", "R7.H095", "R9a", "R10",
                "R11", "R12"):  # fmt: skip
        ours[(row, "")] = (0.5, None if row in ("R3", "R10", "R11", "R12") else 0.4)
        if row != "R6.H095":
            ql[row] = (2.0, 1.9, 1.8)
    for suffix in ("-ours-one-step", "-ours-smoothed"):
        ours[("R9b", suffix)] = (0.5, 0.4)
    ql["R9b"] = (2.0, 1.9, 1.8)
    ours[("R1", "")] = (0.25, 0.2)
    ql["R1"] = (1.0, 0.9, 0.8)
    ql["R3"] = (2.0, 1.9, 0.4)
    return ours, ql


STEPS = {"R1": 10, "R2.K100": 12, "R3": 12, "R4": 50, "R9a": 12}


def speed_row(row, column, median):
    paths = 262144 if row in ("R10", "R11", "R12") else 1048576
    steps = STEPS.get(row, 50)
    cells = [row, column, str(paths), str(steps), repr(median), repr(median), repr(median),
             repr(paths / median), repr(median * 1e9 / (paths * steps)), "3"]  # fmt: skip
    return f"row {row}.{column} " + " ".join(cells)


def speed_file(column, with_ours, reportable="yes", host="mac-m4max", commit=COMMIT, scale=1):
    ours, ql = timings()
    index = {"ql-default": 0, "ql-tuned-nolto": 1, "ql-tuned": 2}[column]
    rows = []
    for row, times in ql.items():
        rows.append(speed_row(row, column, times[index]))
    if with_ours:
        for (row, suffix), (scalar, lanes) in ours.items():
            rows.append(speed_row(row, "ours-scalar" + suffix, scalar))
            if lanes is not None:
                rows.append(speed_row(row, "ours-lanes" + suffix, lanes))
    header = [
        "aadp-results 1",
        "claim: result",
        f"reportable: {reportable}",
        f"reportable_reason: {'none' if reportable == 'yes' else 'iteration'}",
        f"host_label: {host}",
        f"commit: {commit}",
        f"report_tool: aadp_bench_quantlib speed --report --ql x --ours y --repetitions 3 --scale {scale}",
        f"ql_column: {column}",
        "",
        "@section report_ql_speed",
        "type table",
        "title QuantLib against this project on one thread: one timed pricing of every path",
        "targets QL-1,QL-3",
        "timing yes",
        "uses_chain no",
    ]
    header += ["column " + " ".join(c) for c in load_summary().SPEED_COLUMNS]
    return "\n".join(header + rows) + "\n"


_SUMMARY = []


def load_summary():
    if not _SUMMARY:
        _SUMMARY.append(load("quantlib_summary", "scripts/quantlib_summary.py"))
    return _SUMMARY[0]


class Checks:
    def __init__(self):
        self.failures = 0

    def expect(self, what, condition):
        print(("ok: " if condition else "FAILED: ") + what)
        if not condition:
            self.failures += 1

    def near(self, what, got, want):
        self.expect(f"{what}: {got!r} is {want!r}", math.isclose(got, want, rel_tol=1e-12))


def standard_files():
    return [("default", speed_file("ql-default", True)),
            ("nolto", speed_file("ql-tuned-nolto", False)),
            ("tuned", speed_file("ql-tuned", False))]  # fmt: skip


def main():
    summary = load_summary()
    curation = load("curate_quantlib_accuracy", "scripts/curate_quantlib_accuracy.py")
    c = Checks()
    accuracy = accuracy_text(curation)
    s = summary.summarise(accuracy, standard_files())
    rows = {e["label"]: e for e in s["rows"]}
    c.expect("13 rows, R1 to R12 with R9b twice", list(rows) == [r[0] for r in summary.ROWS])

    # R1, by hand. Paths/s: scalar 2^20 / 0.25 = 4194304, lanes 2^20 / 0.2 = 5242880, so lanes
    # are the fastest; QuantLib 2^20 / 1.0 = 1048576 default and 2^20 / 0.8 = 1310720 tuned.
    r1 = rows["R1"]
    c.expect("R1's fastest is the lanes", r1["fastest"] is r1["lanes"])
    c.near("R1 default / scalar", r1["ql"]["ql-default"]["per_scalar"], 0.25)
    c.near("R1 default / fastest", r1["ql"]["ql-default"]["per_fastest"], 0.2)
    c.near("R1 tuned / scalar", r1["ql"]["ql-tuned"]["per_scalar"], 0.3125)
    c.near("R1 tuned / fastest", r1["ql"]["ql-tuned"]["per_fastest"], 0.25)
    # RMSE^2 x time at 2^22 paths: ours fastest 2^-20 x 0.2 s x 4 = 7.62939453125e-07; ours scalar
    # 2^-20 x 1.0 s = 9.5367431640625e-07; QuantLib takes d^2 = 2^-16: default 2^-16 x 4 s =
    # 6.103515625e-05, tuned 2^-16 x 3.2 s = 4.8828125e-05; ratios 80 and 64 to ours fastest, 51.2
    # for tuned to ours scalar.
    c.near("R1 ours fastest RMSE^2 x time", r1["work_fastest"], 7.62939453125e-07)
    c.near("R1 ours scalar RMSE^2 x time", r1["work_scalar"], 9.5367431640625e-07)
    c.near("R1 default RMSE^2 x time", r1["ql"]["ql-default"]["work"], 6.103515625e-05)
    c.near("R1 tuned RMSE^2 x time", r1["ql"]["ql-tuned"]["work"], 4.8828125e-05)
    c.near("R1 default / ours fastest", r1["ql"]["ql-default"]["work_over_fastest"], 80.0)
    c.near("R1 tuned / ours fastest", r1["ql"]["ql-tuned"]["work_over_fastest"], 64.0)
    c.near("R1 tuned / ours scalar", r1["ql"]["ql-tuned"]["work_over_scalar"], 51.2)
    # SE^2 alone: QuantLib 2^-18, so tuned 2^-18 x 3.2 = 1.220703125e-05, ratio 16 to ours fastest
    # (whose SE^2 is its RMSE^2).
    c.near("R1 tuned SE^2 x time", r1["ql"]["ql-tuned"]["se_work"], 1.220703125e-05)
    c.near("R1 tuned SE^2 ratio", r1["ql"]["ql-tuned"]["se_work_over_fastest"], 16.0)
    c.expect("R1 meets QL-1 against both", r1["verdict"] == {"scalar": True, "fastest": True})

    # R3: ours 0.5 s, no lanes; tuned 0.4 s is faster: not met. Both RMSE^2 are 2^-20, so its
    # RMSE^2 x time ratio is 0.4 / 0.5 = 0.8.
    r3 = rows["R3"]
    c.expect("R3's fastest is its scalar", r3["fastest"] is r3["scalar"] and r3["lanes"] is None)
    c.near("R3 tuned / fastest", r3["ql"]["ql-tuned"]["per_fastest"], 1.25)
    c.near("R3 tuned / ours RMSE^2 x time", r3["ql"]["ql-tuned"]["work_over_fastest"], 0.8)
    c.expect("R3 does not meet QL-1", r3["verdict"] == {"scalar": False, "fastest": False})

    # R4: ours 0.5 and 0.4, QuantLib 1.8 tuned, RMSE^2 2^-20 on both: ratio 1.8 / 0.4 = 4.5.
    c.near("R4 tuned / ours RMSE^2 x time", rows["R4"]["ql"]["ql-tuned"]["work_over_fastest"], 4.5)
    c.expect("R4 meets QL-1", rows["R4"]["verdict"] == {"scalar": True, "fastest": True})
    # The rows with no verdict, and R10's speed-only one: 2^18 / 1.8 against 2^18 / 0.5.
    for label in ("R6.H095", "R9a", "R9b (one-step)", "R9b (smoothed)", "R12"):
        c.expect(f"{label} has no verdict", "verdict" not in rows[label])
    c.expect("R6.H095 has no QuantLib column", rows["R6.H095"]["ql"] == {})
    c.near("R10 tuned / scalar", rows["R10"]["ql"]["ql-tuned"]["per_scalar"], 0.5 / 1.8)
    c.expect("R10 meets QL-1 for speed", rows["R10"]["verdict"]["scalar"])
    c.expect("R10 has no RMSE^2 x time", "work_fastest" not in rows["R10"])
    # R9b's two lines share QuantLib's bump: (2^-20 x 7.2 s) / (2^-20 x 1.6 s) = 4.5 each.
    for label in ("R9b (one-step)", "R9b (smoothed)"):
        c.near(f"{label} tuned / ours", rows[label]["ql"]["ql-tuned"]["work_over_fastest"], 4.5)

    text = summary.render(s)
    c.expect("the verdict against scalar counts R3",
             "QL-1 against ours, scalar: NOT MET, on 7 of 8 strictly like-for-like rows." in text)  # fmt: skip
    c.expect("R1's row is rendered with its ratios",
             "| R1 | yes | 4.194 | 5.243 | 1.049 | 1.165 | 1.311 | 0.25 | 0.2 | 0.312 | 0.25 | "
             "7.629e-07 | 6.104e-05 | 5.493e-05 | 4.883e-05 | 80 | 64 | met | met |" in text)  # fmt: skip
    c.expect("the supplement says it has no verdict", "Supplement, no verdict" in text)
    c.expect("the supplement's R1 row", "| R1 | 7.629e-07 | 1.526e-05 | 1.373e-05 | 1.221e-05 | 20 | 16 |" in text)
    c.expect("no iteration banner on reportable files", "ITERATION ONLY" not in text)

    def refused(what, accuracy_text_, files, iteration=False):
        try:
            summary.summarise(accuracy_text_, files, iteration)
        except summary.Refused:
            c.expect(f"refuses {what}", True)
            return
        c.expect(f"refuses {what}", False)

    files = standard_files()
    refused("a column given twice", accuracy, [files[0], files[1], ("again", files[1][1])])
    refused("a missing column", accuracy, files[:2])
    # These two with --iteration, so that the accuracy file's host and commit are not what refuses them.
    refused("two hosts", accuracy, files[:2] + [("x", speed_file("ql-tuned", False, host="linux-zen1"))], True)
    refused("two commits", accuracy, files[:2] + [("x", speed_file("ql-tuned", False, commit="f" * 40))], True)
    refused("an accuracy run at another commit", accuracy_text(curation, commit="e" * 40), files)
    refused("a file that is not reportable", accuracy,
            files[:2] + [("x", speed_file("ql-tuned", False, reportable="no"))])  # fmt: skip
    refused("a reduced size", accuracy, files[:2] + [("x", speed_file("ql-tuned", False, scale=64))])
    refused("an accuracy file validate refuses", accuracy.replace(",0.5,pass,", ",0.7,pass,", 1), files)
    refused("ours in two files", accuracy, files[:2] + [("x", speed_file("ql-tuned", True))])
    refused("a file that is not stamped", accuracy, files[:2] + [("x", "@section report_ql_speed\n")])

    iteration_files = files[:2] + [("x", speed_file("ql-tuned", False, reportable="no", scale=64))]
    s2 = summary.summarise(accuracy, iteration_files, iteration=True)
    c.expect("--iteration prints the banner", summary.render(s2).startswith("**ITERATION ONLY"))

    if c.failures:
        print(f"{c.failures} check(s) failed")
        return 1
    print("all QuantLib summary checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
