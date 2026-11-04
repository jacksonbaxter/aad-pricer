#!/usr/bin/env bash
# End-to-end checks of scripts/run_bench.sh against a stand-in measure.sh, so that they run
# anywhere. Usage: run_bench_test.sh <harness binary> <scratch dir>
set -uo pipefail

binary=$1
scratch=$2
script=scripts/run_bench.sh
fast=(--batches=100 --repetitions=2 --warmup-ms=1 --gb-warmup-s=0.001)
failures=0

# The stamp refuses a niced run, and rightly, so the reportable path below cannot pass from a niced
# shell. zsh runs background jobs at nice 5 by default. One line here, rather than four checks
# failing on a file that was never written.
niceness=$(ps -o ni= -p $$ | tr -d ' ')
if [[ $niceness != 0 ]]; then
  echo "FAILED: run_bench_test needs nice 0, it is at nice ${niceness:-unknown}:" \
    "stamping refuses a niced run, so a clean run cannot be checked. Run it in the foreground."
  exit 1
fi

check() {
  local what=$1
  shift
  if "$@"; then
    echo "ok: $what"
  else
    echo "FAILED: $what"
    failures=$((failures + 1))
  fi
}

mkdir -p "$scratch/source" "$scratch/results"
find "$scratch/results" -type f -delete
find "$scratch/source" -mindepth 1 -delete
export AADP_RESULTS_DIR=$scratch/results
export AADP_SOURCE_ROOT=$scratch/source
printf '0123456789abcdef0123456789abcdef01234567\n' >"$scratch/source/COMMIT"

# A stand-in with the real script's stderr lines.
cat >"$scratch/measure_ok.sh" <<'STUB'
#!/usr/bin/env bash
echo "measure.sh: start load1=0.25 host=$(hostname -s) at=$(date -u +%FT%TZ)" >&2
"$@"
rc=$?
echo "measure.sh: end load1=0.30 exit=$rc at=$(date -u +%FT%TZ)" >&2
exit "$rc"
STUB
cat >"$scratch/measure_busy.sh" <<'STUB'
#!/usr/bin/env bash
echo "measure.sh: lock busy for 3600s; not measuring" >&2
exit 75
STUB
chmod +x "$scratch/measure_ok.sh" "$scratch/measure_busy.sh"

latest() { ls -t "$AADP_RESULTS_DIR"/*.txt 2>/dev/null | head -1; }
clear_results() { find "$AADP_RESULTS_DIR" -type f -delete; }
# Several refusals can share an exit code, so the message has to match as well.
fails_with() {
  local wanted=$1 reason=$2 status
  shift 2
  "$@" >/dev/null 2>"$scratch/refusal.err"
  status=$?
  [[ $status == "$wanted" ]] && grep -qF -- "$reason" "$scratch/refusal.err"
}
has_no_host_field() { ! grep -q 'host=' "$(latest)"; }

# Exit 75 is passed through, and nothing is stamped.
AADP_MEASURE=$scratch/measure_busy.sh "$script" "$binary" script-busy "${fast[@]}" >/dev/null \
  2>"$scratch/busy.err"
check "exit 75 is passed through" test $? -eq 75
check "exit 75 says to retry later" grep -q 'measure.sh says retry later' "$scratch/busy.err"
check "exit 75 stamps nothing" test -z "$(latest)"

# With no measure.sh anywhere, the run goes ahead marked not reportable. A copy of the script
# has no ../.orchestrator beside it, and HOME is moved so the last place finds nothing either.
mkdir -p "$scratch/copy/scripts"
cp "$script" "$scratch/copy/scripts/"
HOME=$scratch AADP_MEASURE= "$scratch/copy/scripts/run_bench.sh" "$binary" script-none "${fast[@]}" \
  >"$scratch/none.out" 2>"$scratch/none.err"
check "a run without measure.sh still completes" test $? -eq 0
check "a run without measure.sh says so" grep -q "measure.sh not found" "$scratch/none.err"
check "the file says not reportable" grep -q '^reportable: no$' "$(latest)"
check "the file says why" grep -q '^reportable_reason: measure.sh not found$' "$(latest)"
clear_results

AADP_NOT_REPORTABLE="iteration run" AADP_MEASURE=$scratch/measure_ok.sh \
  "$script" "$binary" script-iter "${fast[@]}" >/dev/null 2>"$scratch/iter.err"
check "an iteration run completes" test $? -eq 0
check "an iteration run is not reportable" grep -q '^reportable: no$' "$(latest)"
check "an iteration run keeps its reason" grep -q '^reportable_reason: iteration run$' "$(latest)"
check "the load lines are kept" grep -q '^measure_start: measure.sh: start load1=0.25 at=' "$(latest)"
check "the host field is gone" has_no_host_field
clear_results

# Captures stay in the build tree, next to the binary.
bin_dir=$(cd "$(dirname "$binary")" && pwd -P)
check "the capture is under the build tree" \
  test -n "$(find "$(dirname "$bin_dir")/bench-runs" -type d -name 'script-iter-*' | head -1)"

# The binary's own refusal goes to the capture, not to run_bench.sh's stderr.
capture_of() {
  find "$(dirname "$bin_dir")/bench-runs" -path "*$1-*/stderr.txt" | sort | tail -1
}

# A failing benchmark passes its status on and stamps nothing.
AADP_MEASURE=$scratch/measure_ok.sh "$script" "$binary" script-bad --batch-units=1 "${fast[@]}" \
  >/dev/null 2>"$scratch/bad.err"
check "a failed run passes its status on" test $? -eq 5
check "a failed run says so" grep -q 'the run failed with exit 5' "$scratch/bad.err"
check "the capture says why it failed" grep -q 'refusing to produce a histogram' \
  "$(capture_of script-bad)"
check "a failed run stamps nothing" test -z "$(latest)"

# The whole path to a reportable file, which only a Release build on a known host may reach.
AADP_MEASURE=$scratch/measure_ok.sh "$script" "$binary" script-full "${fast[@]}" \
  >/dev/null 2>"$scratch/full.err"
status=$?
raw=$(find "$(dirname "$bin_dir")/bench-runs" -path '*script-full-*/output.txt' | sort | tail -1)
if grep -q '^build_type: Release$' "$raw" && ! grep -q '^host_label: other$' "$raw"; then
  check "a clean run is stamped" test "$status" -eq 0
  check "a clean run is reportable" grep -q '^reportable: yes$' "$(latest)"
  check "a clean run names its commit" \
    grep -q '^commit: 0123456789abcdef0123456789abcdef01234567$' "$(latest)"
  check "a clean run names the exported tree" grep -q '^tree_state: exported$' "$(latest)"
elif grep -q '^host_label: other$' "$raw"; then
  check "host other is not reportable" grep -q '^reportable: no$' "$(latest)"
else
  check "the stamp refuses a build that is not Release" test "$status" -eq 1
  check "the refusal says why" grep -q 'build type' "$scratch/full.err"
  check "a refused run stamps nothing" test -z "$(latest)"
fi
clear_results

# Pins. Only linux-zen1 pins, on one CPU or two (a threaded row), never on cpu5, which is meant
# to be idle; every other host refuses a pin it cannot make.
if [[ $("$bin_dir/aadp_results_stamp" --print-host-label) == linux-zen1 ]]; then
  check "a pin to three CPUs is refused" fails_with 64 'one or two CPUs' env AADP_PIN_CPUS=4,6,7 \
    AADP_MEASURE="$scratch/measure_ok.sh" "$script" "$binary" script-pin "${fast[@]}"
  check "a pin to one CPU twice is refused" fails_with 64 'the same CPU twice' env \
    AADP_PIN_CPUS=6,6 AADP_MEASURE="$scratch/measure_ok.sh" "$script" "$binary" script-pin \
    "${fast[@]}"
  check "a pin on the idle CPU is refused" fails_with 64 'cpu5 is meant to be idle' env \
    AADP_PIN_CPUS=4,5 AADP_MEASURE="$scratch/measure_ok.sh" "$script" "$binary" script-pin \
    "${fast[@]}"
  AADP_PIN_CPUS=4,6 AADP_MEASURE=$scratch/measure_ok.sh "$script" "$binary" script-two \
    "${fast[@]}" >/dev/null 2>"$scratch/two.err"
  two_status=$?
  two_raw=$(find "$(dirname "$bin_dir")/bench-runs" -path '*script-two-*/output.txt' | sort |
    tail -1)
  check "the raw output names both CPUs" grep -q '^pinned_cpus: 4,6$' "$two_raw"
  check "the raw output says cpu5 is meant to be idle" \
    grep -q '^meant_idle_cpus: 5 (not enforced)$' "$two_raw"
  # As for the full run above: only a Release build may be stamped.
  if grep -q '^build_type: Release$' "$two_raw"; then
    check "a two-CPU run is stamped" test "$two_status" -eq 0
    check "the header names both CPUs" grep -q '^pinned_cpus: 4,6$' "$(latest)"
    check "the header says cpu5 is meant to be idle" \
      grep -q '^meant_idle_cpus: 5 (not enforced)$' "$(latest)"
  else
    check "the stamp refuses a two-CPU run that is not Release" test "$two_status" -eq 1
    check "the two-CPU refusal says why" grep -q 'build type' "$scratch/two.err"
    check "a refused two-CPU run stamps nothing" test -z "$(latest)"
  fi
  clear_results
else
  check "a host that cannot pin refuses a pin" fails_with 64 'AADP_PIN_CPUS is for linux-zen1' \
    env AADP_PIN_CPUS=4,6 AADP_MEASURE="$scratch/measure_ok.sh" "$script" "$binary" script-pin \
    "${fast[@]}"
fi

# A dirty tree stops a reportable run before it measures anything.
git -C "$scratch/source" init -q
git -C "$scratch/source" -c user.name=t -c user.email=t@example.invalid commit -q --allow-empty \
  -m first
printf 'x\n' >"$scratch/source/untracked.txt"
if [[ $("$bin_dir/aadp_results_stamp" --print-host-label) != other ]]; then
  check "a dirty tree refuses a reportable run" fails_with 4 'the run failed with exit 4' env \
    AADP_MEASURE="$scratch/measure_ok.sh" "$script" "$binary" script-dirty "${fast[@]}"
  check "the capture says the tree is dirty" grep -q 'reportable run refused: tree is dirty' \
    "$(capture_of script-dirty)"
fi

check "a bad name is refused" fails_with 64 'name must be lower-case' "$script" "$binary" 'Bad/Name'
check "too few arguments are refused" fails_with 64 'usage: run_bench.sh' "$script" "$binary"
check "a missing binary is refused" fails_with 66 'cannot execute the benchmark binary' \
  "$script" "$scratch/no-such-binary" name

if ((failures)); then
  echo "$failures check(s) failed"
  exit 1
fi
echo "all script checks passed"
