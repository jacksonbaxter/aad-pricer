#!/usr/bin/env python3
"""Extracts the hot loops of the AAD benchmark binary, for EVIDENCE B15 and B17.

    python3 -I scripts/extract_asm.py --build <build dir> --label <label> --out <file>

Reads <build dir>/bin/aadp_bench_aad, the binary the benchmark runs, its CMakeCache.txt for the
compiler and SIMD target, and compile_commands.json for the bench unit's code-generation flags. Needs llvm-nm, llvm-cxxfilt and llvm-objdump on PATH (the toolchain's env.sh puts them
there). Standard library only. Exit codes: 0 written, 1 a function or loop was not found, 2 could
not run.

For each function below it finds the loop named, prints a count of its instructions by kind, and
an excerpt. Nothing in the output is a path or a host name: symbols are shortened, addresses are
offsets from the function's start, and of the compile command only flags that are not paths.

  lane-sweep     simd::LaneTape<N lanes>::SweepBack, the vector tape's reverse sweep. The loop over
                 one statement's operands, whole: adjoint times partials, then a masked add into
                 the target's adjoints. Vector means the partials and adjoints move as packed
                 doubles; scalar-with-gathers would show per-lane loads or lane inserts.
  scalar-sweep   ad::Tape::SweepBack, the scalar tape's, for comparison: the same loop.
  lane-primal    simd::lane_detail::OutSmoothed<LaneReal<N lanes>>, AAD-1's kernel on lanes. The
                 loop over the steps: counts for all of it, the first instructions as an excerpt.
  scalar-primal  pricer::kernel_detail::OutSmoothed<double>, the scalar primal of the same kernel:
                 whether the compiler vectorised it (B17).
  scalar-inverse-normal
                 math::inverse_normal, which that loop calls once a step: counts for the whole
                 function, its branches included, and no listing.

A loop is the union of the backward branches' ranges that hold the instruction it is found by:
the first packed (lane) or scalar (scalar) double multiply for a sweep, the widest such range for a
primal. Packed and scalar are told apart by the operands: NEON's .2d arrangement or a q register
against a d register, x86's pd on xmm, ymm or zmm against sd.
"""

import argparse
import json
import re
import shlex
import subprocess
import sys

EXCERPT_LINES = 40

# (name, demangled-name regex with {lanes}, how the loop is found, how much is printed)
TARGETS = [
    ("lane-sweep", r"^aadp::simd::LaneTape<hwy::\w+::Simd<double, {lanes}ul, 0> ?>::SweepBack\(",
     "mul", "all"),
    ("scalar-sweep", r"^aadp::ad::Tape::SweepBack\(", "mul", "all"),
    ("lane-primal", r"aadp::simd::lane_detail::OutSmoothed<aadp::simd::LaneReal<hwy::\w+::Simd<"
     r"double, {lanes}ul, 0> ?>", "widest", "excerpt"),
    ("scalar-primal", r"aadp::pricer::kernel_detail::OutSmoothed<double,", "widest", "excerpt"),
    ("scalar-inverse-normal", r"^aadp::math::inverse_normal\(double\)$", "function", "none"),
]

INSTRUCTION = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
TARGET_ADDRESS = re.compile(r"(?:0x)?([0-9a-f]+)\s*<")
COMMENT = re.compile(r"\s*<[^<>]*(?:<[^<>]*>[^<>]*)*>")


def run(args, stdin=None):
    result = subprocess.run(args, input=stdin, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.stderr.write(f"extract_asm: {args[0]} failed: {result.stderr.strip()[:400]}\n")
        sys.exit(2)
    return result.stdout


def symbols(binary):
    """Mangled name of every defined function, keyed by its demangled name."""
    mangled = []
    for line in run(["llvm-nm", "--defined-only", binary]).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("t", "T", "W", "w"):
            mangled.append(parts[2])
    # Mach-O puts one more underscore before each name than the Itanium mangling has.
    plain = [m[1:] if m.startswith("__Z") else m for m in mangled]
    demangled = run(["llvm-cxxfilt", "-n"], stdin="\n".join(plain) + "\n").splitlines()
    return dict(zip(demangled, mangled))


def disassemble(binary, mangled, x86):
    args = ["llvm-objdump", "-d", "--no-show-raw-insn", f"--disassemble-symbols={mangled}"]
    if x86:
        args.append("--x86-asm-syntax=intel")
    rows = []
    for line in run(args + [binary]).splitlines():
        match = INSTRUCTION.match(line)
        if match:
            rows.append((int(match.group(1), 16), match.group(2), match.group(3).strip()))
    return rows


def is_branch(mnemonic, x86):
    if x86:
        return mnemonic.startswith("j")
    return mnemonic in ("b", "cbz", "cbnz", "tbz", "tbnz") or mnemonic.startswith("b.")


def branch_target(operands):
    match = TARGET_ADDRESS.search(operands)
    return int(match.group(1), 16) if match else None


# Kinds of instruction, by architecture. Each returns a kind or None.
def kind_x86(mnemonic, operands):
    width = "zmm" if "zmm" in operands else "ymm" if "ymm" in operands else "xmm"
    bits = {"xmm": 128, "ymm": 256, "zmm": 512}[width]
    if mnemonic.startswith("vgather") or mnemonic.startswith("gather"):
        return "gather"
    if mnemonic.startswith("call"):
        return "call"
    if re.match(r"v?f(n)?m(add|sub)", mnemonic):
        return "fma"
    arithmetic = re.match(r"v?(mul|add|sub|div|sqrt|min|max)(pd|sd)$", mnemonic)
    if arithmetic:
        if arithmetic.group(2) == "sd":
            return "scalar double arithmetic"
        return f"packed double arithmetic ({bits}-bit)"
    if re.match(r"v?(blendv|blendm|and|andn|or|xor|cmp)pd$", mnemonic) or mnemonic in (
            "vpblendvb", "vpternlogq", "vpandq", "vpand", "vpor", "vpxor", "vpcmpeqq"):
        return f"packed select, compare or logic ({bits}-bit)"
    if re.match(r"v?(insertps|pinsrq|movhpd|movlpd|unpcklpd|unpckhpd|insertf128|insertf64x4|"
                r"pextrq|extractf128|extractf64x4|permpd|permq|shufpd)$", mnemonic):
        return "lane insert, extract or shuffle"
    return None


def kind_arm(mnemonic, operands):
    if mnemonic in ("bl", "blr"):
        return "call"
    base, _, arrangement = mnemonic.partition(".")
    if re.match(r"f(n)?m(add|sub|la|ls)$", base):
        return "fma"
    if re.match(r"f(mul|add|sub|div|sqrt|min|max|minnm|maxnm)$", base):
        if arrangement == "2d":
            return "packed double arithmetic (128-bit)"
        if re.match(r"d\d+", operands):
            return "scalar double arithmetic"
        return None
    if arrangement in ("2d", "16b") and base in ("bif", "bit", "bsl", "and", "orr", "eor", "bic",
                                                 "cmeq", "cmhi", "cmhs", "cmgt", "cmge", "fcmeq",
                                                 "fcmgt", "fcmge"):
        return "packed select, compare or logic (128-bit)"
    if base in ("ins", "ld1", "mov", "dup", "zip1", "zip2", "ext") and "[" in operands:
        return "lane insert, extract or shuffle"
    return None


def find_loop(rows, x86, how):
    """(first, last) row indices of the loop, or None."""
    if how == "function":
        return 0, len(rows) - 1
    index = {address: i for i, (address, _, _) in enumerate(rows)}
    kind = kind_x86 if x86 else kind_arm
    ranges = []
    for i, (address, mnemonic, operands) in enumerate(rows):
        if not is_branch(mnemonic, x86):
            continue
        target = branch_target(operands)
        if target is not None and target <= address and target in index:
            ranges.append((index[target], i))
    if not ranges:
        return None
    if how == "widest":
        return max(ranges, key=lambda r: r[1] - r[0])
    want = "packed double arithmetic" if how == "mul" else how
    for i, (_, mnemonic, operands) in enumerate(rows):
        found = kind(mnemonic, operands) or ""
        is_mul = "mul" in mnemonic and ("arithmetic" in found)
        if is_mul and (found.startswith(want) or found == "scalar double arithmetic"):
            holding = [r for r in ranges if r[0] <= i <= r[1]]
            if not holding:
                continue
            # The innermost: the shortest range, merged with every range that overlaps it and is
            # no longer than twice its length (the operand loop's out-of-line arm).
            inner = min(holding, key=lambda r: r[1] - r[0])
            limit = 2 * (inner[1] - inner[0]) + 2
            first, last = inner
            for r in ranges:
                if r[1] - r[0] <= limit and r[0] <= last and r[1] >= first:
                    first, last = min(first, r[0]), max(last, r[1])
            return first, last
    return None


def shorten(name):
    name = re.sub(r"std::__1::", "std::", name)
    name = re.sub(r"\(.*$", "(...)", name)
    return name


def render(name, demangled, rows, loop, how, x86, names):
    start = rows[0][0]
    first, last = loop
    kind = kind_x86 if x86 else kind_arm
    counts = {}
    for _, mnemonic, operands in rows[first:last + 1]:
        k = kind(mnemonic, operands)
        if k:
            counts[k] = counts.get(k, 0) + 1
        if is_branch(mnemonic, x86) and mnemonic not in ("b", "jmp"):
            counts["conditional branch"] = counts.get("conditional branch", 0) + 1
    what = "function" if how == "none" else "loop"
    out = [f"=== {name}: {shorten(demangled)}",
           f"{what}: +0x{rows[first][0] - start:x} to +0x{rows[last][0] - start:x}, "
           f"{last - first + 1} instructions of the function's {len(rows)}"]
    for k in sorted(counts):
        out.append(f"  {k}: {counts[k]}")
    for k in ("packed double arithmetic", "scalar double arithmetic", "gather", "fma", "call",
              "conditional branch"):
        if not any(c.startswith(k) for c in counts):
            out.append(f"  {k}: 0")
    shown = rows[first:last + 1]
    if how == "none":
        return "\n".join(out) + "\n"
    if how == "excerpt" and len(shown) > EXCERPT_LINES:
        shown = shown[:EXCERPT_LINES]
        out.append(f"first {EXCERPT_LINES} instructions of the loop:")
    else:
        out.append("the whole loop:")
    for address, mnemonic, operands in shown:
        target = branch_target(operands) if is_branch(mnemonic, x86) else None
        callee = re.search(r"<(.*)>", operands) if kind(mnemonic, operands) == "call" else None
        text = COMMENT.sub("", operands)
        if callee:
            symbol = re.sub(r"\+0x[0-9a-f]+$", "", callee.group(1))
            text = shorten(names.get(symbol, symbol))
        if target is not None:
            text = re.sub(r"(?:0x)?[0-9a-f]+$", f"+0x{target - start:x}", text.strip())
        out.append(f"  +0x{address - start:04x}  {mnemonic:<10} {text}".rstrip())
    return "\n".join(out) + "\n"


def is_x86(binary):
    """From the header: ELF e_machine 62, or Mach-O cputype x86_64."""
    with open(binary, "rb") as f:
        head = f.read(20)
    if head[:4] == b"\x7fELF":
        return int.from_bytes(head[18:20], "little") == 62
    return int.from_bytes(head[4:8], "little") == 0x01000007


def compile_flags(build):
    """The code-generation flags the bench's own unit was compiled with: optimisation, target,
    floating point and defines, from compile_commands.json, so the target's -march is included.
    Paths are never among them."""
    with open(f"{build}/compile_commands.json", encoding="utf-8") as f:
        commands = json.load(f)
    entry = next((e for e in commands if e["file"].endswith("bench/aad_workloads.cpp")), None)
    if entry is None:
        sys.stderr.write("extract_asm: bench/aad_workloads.cpp is not in compile_commands.json\n")
        sys.exit(2)
    words = entry.get("arguments") or shlex.split(entry["command"])
    keep = [w for w in words if w.startswith(("-O", "-m", "-ffp", "-DNDEBUG", "-DHWY"))]
    return " ".join(keep)


def cache_value(build, key):
    with open(f"{build}/CMakeCache.txt", encoding="utf-8") as cache:
        for line in cache:
            if line.startswith(key + ":"):
                return line.split("=", 1)[1].strip()
    return ""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    binary = f"{args.build}/bin/aadp_bench_aad"
    target = cache_value(args.build, "AADP_SIMD_TARGET")
    flags = compile_flags(args.build)
    compiler = run([cache_value(args.build, "CMAKE_CXX_COMPILER"), "--version"]).splitlines()[0]
    compiler = re.sub(r"\s*\(http[^)]*\)", "", compiler)
    x86 = is_x86(binary)
    table = symbols(binary)
    names = {mangled: demangled for demangled, mangled in table.items()}
    lanes = None
    for demangled in table:
        match = re.match(r"^aadp::simd::LaneTape<hwy::\w+::Simd<double, (\d+)ul, 0> ?>::SweepBack",
                         demangled)
        if match:
            lanes = max(lanes or 0, int(match.group(1)))
    if lanes is None:
        sys.stderr.write("extract_asm: no lane tape sweep in the binary\n")
        return 1
    parts = [f"# {args.label}: aadp_bench_aad, AADP_SIMD_TARGET={target or 'baseline'}, "
             f"{lanes} double lanes, the widest the build has",
             f"# {compiler}; flags: {flags}",
             "# regenerate: python3 -I scripts/extract_asm.py --build <build dir> "
             f"--label {args.label} --out docs/asm/{args.label}.txt", ""]
    status = 0
    for name, pattern, how, extent in TARGETS:
        regex = re.compile(pattern.format(lanes=lanes))
        found = [d for d in table if regex.search(d)]
        if len(found) != 1:
            sys.stderr.write(f"extract_asm: {name}: {len(found)} functions match, want 1"
                             f" (inlined?): {[shorten(f) for f in found][:4]}\n")
            parts.append(f"=== {name}: not found as one function in this build\n")
            status = 1
            continue
        rows = disassemble(binary, table[found[0]], x86)
        loop = find_loop(rows, x86, how) if rows else None
        if loop is None:
            sys.stderr.write(f"extract_asm: {name}: no loop found\n")
            parts.append(f"=== {name}: no loop found\n")
            status = 1
            continue
        parts.append(render(name, found[0], rows, loop, extent, x86, names))
    with open(args.out, "w", encoding="utf-8") as out:
        out.write("\n".join(parts))
    return status


if __name__ == "__main__":
    sys.exit(main())
