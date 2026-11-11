#!/usr/bin/env python3
"""The QuantLib flag check (scripts/check_quantlib_flags.py) on synthetic compile databases.

    python3 -I tests/bench/quantlib_flags_test.py

Exit 0 when every case gives its expected verdict. Standard library only.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Importing must not write scripts/__pycache__ into the working tree.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(HERE, "..", "..", "scripts"))

import check_quantlib_flags as flags  # noqa: E402

BASE = ["clang++", "-ffp-contract=off", "-O3", "-DNDEBUG", "-std=gnu++23"]
QL = "/b/_deps/quantlib-src/ql/pricingengines/barrier/mcbarrierengine.cpp"


def database(ql_args, bench_args, use_command=False, drop=None):
    entries = []
    units = [QL] + ["/repo/" + unit for unit in flags.BENCH_UNITS]
    for path in units:
        args = list(ql_args if path == QL else bench_args)
        args += ["-o", "x.o", "-c", path]
        if drop is not None and path.endswith(drop):
            continue
        entry = {"directory": "/b", "file": path}
        if use_command:
            entry["command"] = " ".join(args)
        else:
            entry["arguments"] = args
        entries.append(entry)
    return entries


def bench_variant(extra, remove=()):
    return [a for a in BASE if a not in remove] + extra


CASES = [
    ("identical", database(BASE, BASE), 0),
    ("identical, command strings", database(BASE, BASE, use_command=True), 0),
    ("warnings, includes and definitions differ",
     database(BASE + ["-Wall", "-Wno-unknown-pragmas", "-DBOOST_ALL_NO_LIB", "-isystem", "/x"],
              BASE + ["-Wall", "-Wextra", "-Werror", "-I", "/y", "-DAADP=1"]), 0),
    ("bench at -O2", database(BASE, bench_variant(["-O2"], remove=("-O3",))), 1),
    ("bench adds a later -O1", database(BASE, bench_variant(["-O1"])), 1),
    ("QuantLib without -ffp-contract=off",
     database([a for a in BASE if a != "-ffp-contract=off"], BASE), 1),
    ("LTO on QuantLib only", database(BASE + ["-flto=thin"], BASE), 1),
    ("a CPU on the bench only", database(BASE, bench_variant(["-mcpu=apple-m4"])), 1),
    ("AVX2 on QuantLib only", database(BASE + ["-mavx2", "-mfma"], BASE), 1),
    ("NDEBUG missing on the bench", database(BASE, bench_variant([], remove=("-DNDEBUG",))), 1),
    ("fast math on the bench", database(BASE, bench_variant(["-ffast-math"])), 1),
    ("different -arch", database(BASE + ["-arch", "arm64"], BASE + ["-arch", "x86_64"]), 1),
    ("no QuantLib unit", database(BASE, BASE, drop="mcbarrierengine.cpp"), 2),
    ("a bench unit missing", database(BASE, BASE, drop="quantlib_side.cpp"), 2),
]


def main():
    failures = 0
    for name, db, expected in CASES:
        status, lines = flags.check(db)
        verdict = "ok" if status == expected else "FAILED"
        if status != expected:
            failures += 1
        print(f"{verdict:6} {name}: exit {status}, expected {expected}")
        if status != expected:
            for line in lines:
                print("       " + line)
    print(f"quantlib_flags_test: {len(CASES) - failures} of {len(CASES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
