#!/usr/bin/env python3
"""Fails unless QuantLib and the comparison bench were compiled with the same optimisation and
architecture flags (NOTES.md, "Stage 10").

    python3 -I scripts/check_quantlib_flags.py <build>/compile_commands.json <quantlib source dir>

The QuantLib source directory is CMake's quantlib_SOURCE_DIR, which the build passes. FetchContent
puts it under <build>/_deps/quantlib-src by default, but FETCHCONTENT_SOURCE_DIR_QUANTLIB moves it
anywhere (the fault runner's shared cache does), so the sampled unit is looked up by its full path
there, never by a guessed suffix.

Reads one sampled QuantLib translation unit and every bench translation unit from the compile
database, keeps the flags that decide code generation, in order, and compares the lists:
  -O*, -march=, -mcpu=, -mtune= and every other -m flag, -arch <x>, --target= and -target <x>,
  -ffp-contract=, -ffast-math and its parts, the denormal mode, -flto* and -fno-lto,
  -DNDEBUG and -UNDEBUG.
Warnings, include paths, definitions other than NDEBUG and the language standard do not matter.

Exit codes: 0 same flags, 1 a mismatch, 2 could not check (a unit missing, a bad database).
Standard library only.
"""
import json
import os
import re
import shlex
import sys

QUANTLIB_SAMPLE = "ql/pricingengines/barrier/mcbarrierengine.cpp"  # Relative to the source dir.
BENCH_UNITS = (
    "bench/quantlib/main.cpp",
    "bench/quantlib/quantlib_side.cpp",
    "bench/quantlib/ours.cpp",
    "bench/quantlib/local_vol_rows.cpp",
    "src/pricer/pricer.cpp",
    "src/simd/simd_pricer.cpp",
)

_SINGLE = re.compile(
    r"^(-O.*|-m.+|--target=.+|-ffp-contract=.+|-f(no-)?fast-math|-f(no-)?unsafe-math-optimizations"
    r"|-f(no-)?associative-math|-f(no-)?reciprocal-math|-f(no-)?signed-zeros"
    r"|-f(no-)?trapping-math|-f(no-)?finite-math-only|-f(no-)?math-errno|-f(no-)?approx-func"
    r"|-f(no-)?honor-(nans|infinities)|-fdenormal-fp-math.*|-ffp-model=.+|-ffp-exception-behavior=.+"
    r"|-flto.*|-fno-lto|-[DU]NDEBUG)$"
)
_WITH_VALUE = ("-arch", "-target")


def arguments(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def codegen_flags(args):
    """The code-generation flags of one compile, in command-line order."""
    kept = []
    i = 1  # args[0] is the compiler
    while i < len(args):
        arg = args[i]
        if arg in _WITH_VALUE and i + 1 < len(args):
            kept.append(arg + " " + args[i + 1])
            i += 2
            continue
        if arg in ("-o", "-c", "-MF", "-MT", "-MQ", "-I", "-isystem", "-include", "-x"):
            i += 2
            continue
        if _SINGLE.match(arg):
            kept.append(arg)
        i += 1
    return kept


def unit_path(entry):
    path = entry.get("file", "")
    if not os.path.isabs(path):
        path = os.path.join(entry.get("directory", ""), path)
    return path.replace("\\", "/")


def same_file(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def find_unit(database, suffix):
    for entry in database:
        if unit_path(entry).endswith("/" + suffix):
            return entry
    return None


def find_quantlib_unit(database, quantlib_source, sample):
    target = os.path.join(quantlib_source, sample)
    for entry in database:
        if same_file(unit_path(entry), target):
            return entry
    return None


def check(database, quantlib_source, quantlib_sample=QUANTLIB_SAMPLE, bench_units=BENCH_UNITS):
    """Returns (exit code, report lines)."""
    lines = []
    sample = find_quantlib_unit(database, quantlib_source, quantlib_sample)
    if sample is None:
        where = os.path.join(quantlib_source, quantlib_sample)
        return 2, ["check_quantlib_flags: no QuantLib unit " + where]
    reference = codegen_flags(arguments(sample))
    lines.append("quantlib " + quantlib_sample + ": " + " ".join(reference))
    status = 0
    for unit in bench_units:
        entry = find_unit(database, unit)
        if entry is None:
            return 2, lines + ["check_quantlib_flags: no bench unit " + unit]
        flags = codegen_flags(arguments(entry))
        if flags == reference:
            lines.append("same     " + unit)
        else:
            lines.append("flag mismatch " + unit + ": " + " ".join(flags))
            status = 1
    lines.append("check_quantlib_flags: " + ("same flags" if status == 0 else "flag mismatch"))
    return status, lines


def main(argv):
    if len(argv) != 3 or not argv[2]:
        print(__doc__.strip().splitlines()[3].strip(), file=sys.stderr)
        return 2
    try:
        with open(argv[1], encoding="utf-8") as handle:
            database = json.load(handle)
    except (OSError, ValueError) as error:
        print("check_quantlib_flags: cannot read " + argv[1] + ": " + str(error), file=sys.stderr)
        return 2
    if not isinstance(database, list):
        print("check_quantlib_flags: not a compile database", file=sys.stderr)
        return 2
    status, lines = check(database, argv[2])
    for line in lines:
        print(line)
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
