#!/usr/bin/env bash
# scripts/run_bench.sh with a report emitter in place of a benchmark binary: the same measure.sh
# lines, run flags, refusals and stamp, against a stand-in measure.sh so that it runs anywhere.
# Usage: run_bench_report_test.sh <report emitter> <scratch dir>
set -uo pipefail

binary=$1
scratch=$2
script=scripts/run_bench.sh
failures=0

niceness=$(ps -o ni= -p $$ | tr -d ' ')
if [[ $niceness != 0 ]]; then
  echo "FAILED: run_bench_report_test needs nice 0, it is at nice ${niceness:-unknown}:" \
    "stamping refuses a niced run. Run it in the foreground."
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

cat >"$scratch/measure_ok.sh" <<'STUB'
#!/usr/bin/env bash
echo "measure.sh: start load1=0.25 host=$(hostname -s) at=$(date -u +%FT%TZ)" >&2
"$@"
rc=$?
echo "measure.sh: end load1=0.30 exit=$rc at=$(date -u +%FT%TZ)" >&2
exit "$rc"
STUB
chmod +x "$scratch/measure_ok.sh"

latest() { ls -t "$AADP_RESULTS_DIR"/*.txt 2>/dev/null | head -1; }
clear_results() { find "$AADP_RESULTS_DIR" -type f -delete; }
bin_dir=$(cd "$(dirname "$binary")" && pwd -P)
capture_of() {
  find "$(dirname "$bin_dir")/bench-runs" -path "*$1-*/stderr.txt" | sort | tail -1
}
raw_of() {
  find "$(dirname "$bin_dir")/bench-runs" -path "*$1-*/output.txt" | sort | tail -1
}

# An iteration run: stamped, not reportable, a report file with no benchmark in it.
AADP_NOT_REPORTABLE=iteration AADP_MEASURE=$scratch/measure_ok.sh \
  "$script" "$binary" report-iter --repetitions=2 >/dev/null 2>"$scratch/iter.err"
check "an iteration report run completes" test $? -eq 0
check "it is not reportable" grep -q '^reportable: no$' "$(latest)"
check "it keeps its reason" grep -q '^reportable_reason: iteration$' "$(latest)"
check "it is a report file" grep -q '^@section report_selftest_values$' "$(latest)"
check "with its timed section" grep -q '^timing yes$' "$(latest)"
check "and no benchmark" test -z "$(grep '^@section benchmark_json$' "$(latest)")"
check "the run flags reached it" \
  grep -q '^commit: 0123456789abcdef0123456789abcdef01234567$' "$(latest)"
check "the tree is the exported one" grep -q '^tree_state: exported$' "$(latest)"
check "the load lines are kept" grep -q '^measure_start: measure.sh: start load1=0.25 at=' "$(latest)"
check "the repetitions are in the header" grep -q '^repetitions: 2$' "$(latest)"
clear_results

# The emitter's own usage refusal passes through, and nothing is stamped.
AADP_MEASURE=$scratch/measure_ok.sh "$script" "$binary" report-bad --repetitions=0 \
  >/dev/null 2>"$scratch/bad.err"
check "a refused report run passes its status on" test $? -eq 2
check "the capture says why" grep -q 'bad option --repetitions=0' "$(capture_of report-bad)"
check "a refused report run stamps nothing" test -z "$(latest)"

# The whole path to a reportable report file, which only a Release build on a known host may reach.
AADP_MEASURE=$scratch/measure_ok.sh "$script" "$binary" report-full >/dev/null 2>"$scratch/full.err"
status=$?
raw=$(raw_of report-full)
if grep -q '^build_type: Release$' "$raw" && ! grep -q '^host_label: other$' "$raw"; then
  check "a clean report run is stamped" test "$status" -eq 0
  check "a clean report run is reportable" grep -q '^reportable: yes$' "$(latest)"
  check "its sections are kept" grep -q '^@section report_selftest_timing$' "$(latest)"
elif grep -q '^host_label: other$' "$raw"; then
  check "host other is not reportable" grep -q '^reportable: no$' "$(latest)"
else
  check "the stamp refuses a report build that is not Release" test "$status" -eq 1
  check "the refusal says why" grep -q 'build type' "$scratch/full.err"
  check "a refused report run stamps nothing" test -z "$(latest)"
fi
clear_results

# A dirty tree stops a reportable report run before it computes anything.
git -C "$scratch/source" init -q
git -C "$scratch/source" -c user.name=t -c user.email=t@example.invalid commit -q --allow-empty \
  -m first
printf 'x\n' >"$scratch/source/untracked.txt"
if [[ $("$bin_dir/aadp_results_stamp" --print-host-label) != other ]]; then
  AADP_MEASURE=$scratch/measure_ok.sh "$script" "$binary" report-dirty >/dev/null \
    2>"$scratch/dirty.err"
  check "a dirty tree refuses a reportable report run" test $? -eq 4
  check "the capture says the tree is dirty" grep -q 'reportable run refused: tree is dirty' \
    "$(capture_of report-dirty)"
  check "a dirty report run stamps nothing" test -z "$(latest)"
fi

if ((failures)); then
  echo "$failures check(s) failed"
  exit 1
fi
echo "all report script checks passed"
