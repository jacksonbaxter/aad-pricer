#!/usr/bin/env bash
# aadp_bench_implied_vol (SVI-2) through scripts/run_bench.sh, against a stand-in measure.sh so that
# it runs anywhere: an iteration run is stamped as a benchmark file with the run header, its two
# benchmarks keep their names and one set of quotes a batch, and the table tool renders it.
# Usage: run_bench_implied_vol_test.sh <aadp_bench_implied_vol> <scratch dir>
set -uo pipefail

binary=$1
scratch=$2
script=scripts/run_bench.sh
fast=(--batches=50 --repetitions=2 --warmup-ms=1 --gb-warmup-s=0.001)
failures=0

# The stamp refuses a niced run, so nothing below can pass from a niced shell.
niceness=$(ps -o ni= -p $$ | tr -d ' ')
if [[ $niceness != 0 ]]; then
  echo "FAILED: run_bench_implied_vol_test needs nice 0, it is at nice ${niceness:-unknown}:" \
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

# A stand-in with the real script's stderr lines.
printf '%s\n' '#!/usr/bin/env bash' \
  'echo "measure.sh: start load1=0.25 host=$(hostname -s) at=$(date -u +%FT%TZ)" >&2' \
  '"$@"' 'rc=$?' \
  'echo "measure.sh: end load1=0.30 exit=$rc at=$(date -u +%FT%TZ)" >&2' \
  'exit "$rc"' >"$scratch/measure_ok.sh"
chmod +x "$scratch/measure_ok.sh"

bin_dir=$(cd "$(dirname "$binary")" && pwd -P)
table=$bin_dir/aadp_results_table

AADP_NOT_REPORTABLE=iteration AADP_MEASURE=$scratch/measure_ok.sh \
  "$script" "$binary" svi2-iter "${fast[@]}" >/dev/null 2>"$scratch/iter.err"
check "an iteration run completes and is stamped" test $? -eq 0
file=$(ls -t "$AADP_RESULTS_DIR"/svi2-iter-*.txt 2>/dev/null | head -1)
check "a stamped file is written" test -n "$file"
file=${file:-/nonexistent}
check "it is not reportable" grep -q '^reportable: no$' "$file"
check "it keeps its reason" grep -q '^reportable_reason: iteration$' "$file"
check "it has the run header" grep -q '^commit: 0123456789abcdef0123456789abcdef01234567$' "$file"
check "the load lines are kept" grep -q '^measure_start: measure.sh: start load1=0.25 at=' "$file"
check "its claim is a result" grep -q '^claim: result$' "$file"
check "the repetitions are in the header" grep -q '^repetitions: 2$' "$file"
check "it is a benchmark file" grep -q '^@section benchmark_json$' "$file"
for name in bm_impliedvolatility bm_black76price; do
  check "$name has its histogram" grep -q "^@section histogram_$name\$" "$file"
  check "$name is a Google Benchmark run of that name" \
    grep -q "\"run_name\": \"$name/iterations:50/repeats:2/manual_time\"" "$file"
  # One unit, and one iteration, is one pass over the 400 quotes, as before the harness.
  check "$name times one set of quotes a batch" \
    test "$(awk -v s="@section histogram_$name" '$0 == s { f = 1; next }
      f && /^@section / { exit } f && $1 == "batch_units" { print $2; exit }' "$file")" = 1
done
check "the quotes are described" grep -q '^bench_quotes: 400 out-of-the-money Black-76 quotes' \
  "$file"
check "every quote inverted" grep -q '^bench_inversion_check: pass' "$file"

"$table" --iteration "$file" >"$scratch/table.md" 2>"$scratch/table.err"
check "the table tool renders it as iteration" test $? -eq 0
check "under the iteration banner" grep -q '^\*\*ITERATION ONLY: not results.\*\*' "$scratch/table.md"
for name in bm_impliedvolatility bm_black76price; do
  # 2 repetitions of 50 batches each.
  check "with a row for $name" grep -q "^| $name | 2 | 100 | " "$scratch/table.md"
done
"$table" "$file" >/dev/null 2>"$scratch/table-plain.err"
check "the table tool without --iteration refuses it" test $? -ne 0
check "and says why" grep -q 'not reportable: iteration' "$scratch/table-plain.err"

if ((failures)); then
  echo "$failures check(s) failed"
  exit 1
fi
echo "all implied-vol bench script checks passed"
