#!/usr/bin/env python3
"""The QuantLib accuracy run's curation (scripts/curate_quantlib_accuracy.py) on synthetic output.

    python3 -I tests/bench/quantlib_accuracy_curation_test.py

A raw output of the registered shape, written here with made-up numbers for the 42 comparisons of
bench/quantlib/ceilings.csv, must curate into the expected CSV rows, check against them byte for
byte and validate; every departure from the registered run, and every curated row whose verdict,
bound or z its own cells contradict, must be refused. Last, references/quantlib_accuracy.csv is
validated if it has been committed. Exit 0 if all hold.
"""

import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
COMMITTED = "references/quantlib_accuracy.csv"
CRITICAL = 3.897574976


def load_module():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(
        "curate_quantlib_accuracy", ROOT / "scripts" / "curate_quantlib_accuracy.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def g10(x):
    return f"{x:.10g}"


def names():
    lines = (ROOT / "bench/quantlib/ceilings.csv").read_text(encoding="utf-8").splitlines()
    rows = [line for line in lines if line and not line.startswith("#")]
    return [row.split(",", 1)[0] for row in rows[1:]]


# Every row passes at z = +0.5 (even rows) or -0.25 (odd rows), with se 1/1024, so each number
# is exact in binary and its %.10g text is short. Row 0, R1/quantlib, worked by hand: reference 1,
# se 0.0009765625, estimate 1 + 0.5 se = 1.00048828125, printed 1.000488281; b 0; bound
# 3.897574976 / 1024 = 0.003806225563 (to 10 digits); z 0.5.
def accuracy_rows():
    rows = []
    for i, name in enumerate(names()):
        reference = 1.0 + i / 8.0
        se = 1.0 / 1024.0
        z = 0.5 if i % 2 == 0 else -0.25
        b = 0.0 if i % 3 == 0 else 1e-9
        source = "references/oracle_values.csv O4"
        if name.startswith("R5.H090"):
            source = "closed form, src/oracle/barrier.hpp"
        rows.append(
            [name, g10(reference + z * se), g10(se), g10(reference), g10(b), "0.002",
             g10(b + CRITICAL * se), g10(z), "pass", "lanes-equal-scalar bits", source]
        )  # fmt: skip
    return rows


FIRST_ROW = (
    "R1/quantlib,1.000488281,0.0009765625,1,0,0.002,0.003806225563,0.5,pass,"
    "lanes-equal-scalar bits,references/oracle_values.csv O4"
)


def raw_output(rows=None, **changes):
    rows = accuracy_rows() if rows is None else rows
    header = {
        "title": "# aadp_bench_quantlib accuracy (docs/NOTES.md, Stage 10). The registered "
        "accuracy run (QL-2) at seed 20261112, N = 2^22, when its tree is clean or exported",
        "commit": "# commit: 0123456789abcdef0123456789abcdef01234567",
        "tree": "# tree: exported (COMMIT file)",
        "registered": f"# critical {CRITICAL}, K 42, B 64, N 4194304, seed 20261112",
        "ceilings": "# ceilings: bench/quantlib/ceilings.csv",
        "tally": "# comparisons failed 0, checks failed 0",
    }
    header.update(changes)
    lines = [
        header["title"],
        header["commit"],
        header["tree"],
        "# host: mac-m4max",
        "# cpu: made up for this test",
        "# compiler: Clang 23.1.4",
        "# flags: -ffp-contract=off -O3 -DNDEBUG (Release, C++23)",
        "# link: QuantLib static library; LTO no",
        "# QL_VERSION: 1.44",
        "# QL_HEX_VERSION: 0x014400f0",
        "# BOOST_LIB_VERSION: 1_92",
        "# BOOST_VERSION: 109200",
        "# cache QL_BUILD_EXAMPLES=OFF",
        "# R9a reference: made up",
        "# R9b reference: made up",
        header["ceilings"],
        header["registered"],
        "accuracy,name,estimate,se,reference,b,ceiling,bound,z,verdict,check,reference_source",
    ]
    lines += ["accuracy," + ",".join(row) for row in rows]
    lines += [
        "check,grid-rule-equals-FixedLocalVolSurface,held,largest relative difference 0",
        "sanity,R10,speed-only row: made up",
        "sanity,R11,speed-only row: made up",
        "sanity,R12,made up; no pass mark",
    ]
    if header["tally"]:
        lines.append(header["tally"])
    return "\n".join(lines) + "\n"


class Checks:
    def __init__(self, module, scratch):
        self.module = module
        self.scratch = scratch
        self.failures = 0

    def expect(self, what, condition):
        print(("ok: " if condition else "FAILED: ") + what)
        if not condition:
            self.failures += 1

    def run(self, *argv):
        return self.module.main(["curate_quantlib_accuracy.py", *argv])

    def write(self, name, text):
        path = self.scratch / name
        path.write_text(text, encoding="utf-8")
        return str(path)

    def refused_raw(self, what, text):
        raw = self.write("refused.txt", text)
        out = str(self.scratch / "refused.csv")
        self.expect(f"curate refuses {what}", self.run("curate", raw, out) == 1)

    def refused_curated(self, what, text):
        self.expect(f"validate refuses {what}", self.run("validate", self.write("bad.csv", text)) == 1)


def main():
    module = load_module()
    scratch = ROOT / "build" / "test-scratch" / "quantlib-curation"
    scratch.mkdir(parents=True, exist_ok=True)
    c = Checks(module, scratch)

    raw = c.write("raw.txt", raw_output())
    curated = str(scratch / "curated.csv")
    c.expect("the registered run curates", c.run("curate", raw, curated) == 0)
    text = pathlib.Path(curated).read_text(encoding="utf-8")
    data = [line for line in text.splitlines() if not line.startswith("#")]
    c.expect("the column row comes first", data[0] == ",".join(module.COLUMNS))
    c.expect("42 data rows", len(data) == 43)
    c.expect("the first row is the hand-worked one", data[1] == FIRST_ROW)
    c.expect(
        "a source with a comma is quoted",
        any(line.endswith(',"closed form, src/oracle/barrier.hpp"') for line in data),
    )
    c.expect("the header names the commit and tree",
             "# Commit 0123456789abcdef0123456789abcdef01234567, tree exported, host mac-m4max;"
             in text)  # fmt: skip
    c.expect("the header has the registered run", module.REGISTERED_CURATED in text)
    c.expect("the header says no row uses the chain", "No row is derived from the option chain" in text)
    c.expect("the sanity lines are kept", "# sanity,R12,made up; no pass mark" in text)
    c.expect("the curated file validates", c.run("validate", curated) == 0)
    c.expect("check finds the rows identical", c.run("check", raw, curated) == 0)
    changed = c.write("changed.csv", text.replace("1.000488281", "1.000488282", 1))
    c.expect("check finds a changed row", c.run("check", raw, changed) == 1)
    c.expect("validate-if-present passes on an absent file",
             c.run("validate-if-present", str(scratch / "absent.csv")) == 0)  # fmt: skip
    c.expect("validate-if-present validates a present file",
             c.run("validate-if-present", curated) == 0)  # fmt: skip

    # Not the registered run.
    rows = accuracy_rows()
    c.refused_raw("another seed", raw_output(registered=module.REGISTERED.replace("20261112", "20261113")))
    c.refused_raw("another N", raw_output(registered=module.REGISTERED.replace("4194304", "65536")))
    c.refused_raw("a dirty tree", raw_output(tree="# tree: dirty (git)"))
    c.refused_raw("another ceilings file", raw_output(ceilings="# ceilings: other.csv"))
    c.refused_raw("a short commit", raw_output(commit="# commit: 01f467d"))
    c.refused_raw("a missing row", raw_output(rows=rows[:-1]))
    c.refused_raw("a repeated row", raw_output(rows=rows + rows[-1:]))
    c.refused_raw("rows out of order", raw_output(rows=[rows[1], rows[0]] + rows[2:]))
    c.refused_raw("a run that did not end", raw_output(tally=""))
    c.refused_raw("a tally the rows contradict", raw_output(tally="# comparisons failed 1, checks failed 0"))
    c.refused_raw("an error line", raw_output().replace("check,grid", "error,R1/ours,refused\ncheck,grid"))
    c.refused_raw("anything naming the chain's data", raw_output().replace("made up for this test", "data/DailyMarketReportOpt.csv"))
    c.refused_raw("the smoke run", raw_output(title="# aadp_bench_quantlib smoke (docs/NOTES.md, Stage 10). x"))

    # A curated row its own cells contradict.
    bound0 = "0.003806225563"
    beyond = FIRST_ROW.replace("1.000488281,", "1.004,").replace(",0.5,pass,", ",4.096,pass,")
    c.refused_curated("a pass beyond the bound", text.replace(FIRST_ROW, beyond, 1))
    c.refused_curated("a wrong z", text.replace(",0.5,pass,", ",0.6,pass,", 1))
    c.refused_curated("a wrong bound", text.replace(bound0, "0.004806225563", 1))
    c.refused_curated("a pass over the ceiling", text.replace(",0.002,", ",0.0009,", 1))
    c.refused_curated("a row out of order", text.replace("R1/quantlib,", "R1/ours,", 1))
    c.refused_curated("another seed", text.replace("seed 20261112", "seed 20261113"))
    present_bad = c.write("present-bad.csv", text.replace(",0.5,pass,", ",0.6,pass,", 1))
    c.expect("validate-if-present refuses a present bad file",
             c.run("validate-if-present", present_bad) == 1)  # fmt: skip
    c.refused_curated("a tally the rows contradict",
                      text.replace("# comparisons failed 0,", "# comparisons failed 2,"))  # fmt: skip

    # The curated file, once the reported run is committed.
    c.expect(f"{COMMITTED} is valid or not yet curated", c.run("validate-if-present", str(ROOT / COMMITTED)) == 0)

    if c.failures:
        print(f"{c.failures} check(s) failed")
        return 1
    print("all QuantLib accuracy curation checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
