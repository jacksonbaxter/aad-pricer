#!/usr/bin/env python3
"""Run 2's curation (NOTES.md, "Stage 8") on a synthetic runner output.

    python3 -I tests/oracle/compare_measured_test.py

scripts/compare_measured.py must turn every judged row line, pass, FAIL or UNRESOLVED, into run
1's columns with its verdict, and read nothing else: not the tally lines under the surface rows,
the not-in-run-1 lines of the reference estimator, skips or the summary. Exit 0 if it does.
"""

import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

RUNNER_OUTPUT = """ESTIMATOR: production (made up for this test)
K=4 declared=4 alpha_F=0.01 B=64 critical=4.266218
VA.value           estimate=10.0000000000 reference=10.0000000001 se=1.0000e-03 bound=4.2662e-03 pass condition=none
P3.KID.vega        estimate=11.1000000000 reference=11.1480756406 se=1.0000e-03 bound=4.2672e-03 FAIL condition=distance
P3.AS.d05.a        estimate=0.5000000000 reference=0.5000000000 se=1.0000e-06 bound=1.0000e-05 pass condition=none
  tally P3.AS.d05.a: base butterfly 0 capped 52; +h butterfly +0 capped +0; -h butterfly +0 capped +0
P3.CB.d12.m        estimate=-0.1000000000 reference=-0.1000000100 se=2.0000e-06 bound=8.5324e-06 UNRESOLVED condition=unresolved
  tally P3.CB.d12.m: base butterfly 0 capped 52; +h butterfly +1 capped +0; -h butterfly +0 capped +0
GA.value           not-in-run-1
DB095.value        SKIPPED: no data
run=4 passed=2 failed=1 skipped=1 awaiting=0 K=4
unresolved=1 run2_only=0
"""

EXPECTED = [
    "VA.value,10.0000000000,10.0000000001,1.0000e-03,4.2662e-03,pass",
    "P3.KID.vega,11.1000000000,11.1480756406,1.0000e-03,4.2672e-03,FAIL",
    "P3.AS.d05.a,0.5000000000,0.5000000000,1.0000e-06,1.0000e-05,pass",
    "P3.CB.d12.m,-0.1000000000,-0.1000000100,2.0000e-06,8.5324e-06,UNRESOLVED",
]


def load_module():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(
        "compare_measured", ROOT / "scripts" / "compare_measured.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    module = load_module()
    produced = module.runner_rows(RUNNER_OUTPUT)
    if produced != EXPECTED:
        print("compare_measured_test: the curated rows differ", file=sys.stderr)
        for line in produced:
            print(f"  produced {line}", file=sys.stderr)
        for line in EXPECTED:
            print(f"  expected {line}", file=sys.stderr)
        return 1
    committed = "# header\nid,estimate,reference,se,bound,verdict\n" + "\n".join(EXPECTED) + "\n"
    if module.committed_rows(committed) != EXPECTED:
        print("compare_measured_test: the committed rows did not read back", file=sys.stderr)
        return 1
    print(f"compare_measured_test: {len(EXPECTED)} rows curated with their verdicts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
