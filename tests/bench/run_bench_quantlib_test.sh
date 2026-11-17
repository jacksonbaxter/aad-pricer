#!/usr/bin/env bash
# aadp_bench_quantlib's speed run (QL-1) as a report file through scripts/run_bench.sh, against a
# stand-in measure.sh so that it runs anywhere the quantlib preset builds: an iteration run at a
# reduced size is stamped as a report file with the run header and QuantLib's version record, its
# table holds a row per timed estimator with the text's figures, the table tool renders it, and the
# run flags belong to the report mode alone.
# Usage: run_bench_quantlib_test.sh <aadp_bench_quantlib> <scratch dir>
set -uo pipefail

binary=$1
scratch=$2
script=scripts/run_bench.sh
small=(speed --report --scale 64 --repetitions 1)
failures=0

# The stamp refuses a niced run, so nothing below can pass from a niced shell.
niceness=$(ps -o ni= -p $$ | tr -d ' ')
if [[ $niceness != 0 ]]; then
  echo "FAILED: run_bench_quantlib_test needs nice 0, it is at nice ${niceness:-unknown}:" \
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
  "$script" "$binary" ql-speed-iter "${small[@]}" >/dev/null 2>"$scratch/iter.err"
check "an iteration run completes and is stamped" test $? -eq 0
file=$(ls -t "$AADP_RESULTS_DIR"/ql-speed-iter-*.txt 2>/dev/null | head -1)
check "a stamped file is written" test -n "$file"
file=${file:-/nonexistent}
check "it is not reportable" grep -q '^reportable: no$' "$file"
check "it keeps its reason" grep -q '^reportable_reason: iteration$' "$file"
check "it has the run header" grep -q '^commit: 0123456789abcdef0123456789abcdef01234567$' "$file"
check "the load lines are kept" grep -q '^measure_start: measure.sh: start load1=0.25 at=' "$file"
check "its claim is a result" grep -q '^claim: result$' "$file"
check "the repetitions are in the header" grep -q '^repetitions: 1$' "$file"
check "it is a report file" grep -q '^@section report_ql_speed$' "$file"
check "with no benchmark section" test "$(grep -c '^@section benchmark_json$' "$file")" = 0
check "and no attribution, since nothing comes from the chain" \
  test "$(grep -c '^@section attribution$' "$file")" = 0
check "the table says it is timed" grep -q '^timing yes$' "$file"
check "and not derived from the chain" grep -q '^uses_chain no$' "$file"
check "it names QL-1" grep -q '^targets QL-1,QL-3$' "$file"
check "the header records QuantLib's version" \
  grep -q '^ql_version_record: QL_VERSION 1.44; QL_HEX_VERSION 0x014400f0;' "$file"
check "and its cache values" grep -q '^ql_cache_values: QL_' "$file"
check "and the column" grep -q '^ql_column: ql-default$' "$file"
check "and the reduced size" grep -q '^report_tool: .* --scale 64$' "$file"
check "and the paths it timed" grep -q '^report_timing: .* 16384 paths on R1 to R9 and 4096' \
  "$file"

# Every row of the text the run printed on stderr is a row of the table, with the same figures:
# the text at %.10g, the table at the double's own spelling, so compare the text's spellings.
raw=$(ls -td "$bin_dir"/../bench-runs/ql-speed-iter-* 2>/dev/null | head -1)
text_lines=$(grep -c '^speed,R' "$raw/stderr.txt" 2>/dev/null)
row_lines=$(grep -c '^row ' "$file")
check "the text went to stderr" test "${text_lines:-0}" -gt 0
check "a table row for every text line" test "${text_lines:-0}" = "$row_lines"
check "33 estimators in all at the default columns" test "$row_lines" = 33
mismatch=0
while IFS=, read -r _ row column paths steps median _; do
  got=$(awk -v k="$row.$column" '$1 == "row" && $2 == k { print $5, $6, $7; exit }' "$file")
  # shellcheck disable=SC2086 # three words, split on purpose
  set -- $got
  got_paths=${1:-} got_steps=${2:-} got_median=${3:-}
  # A %.10g text and the cell name the same double to ten digits.
  if [[ $got_paths != "$paths" || $got_steps != "$steps" ]] ||
    ! awk -v a="$median" -v b="$got_median" 'BEGIN { d = a - b; if (d < 0) d = -d;
      exit !(d <= 1e-9 * (b < 0 ? -b : b)) }'; then
    mismatch=$((mismatch + 1))
  fi
done < <(grep '^speed,R' "$raw/stderr.txt")
check "each row has the text's paths, steps and median" test "$mismatch" = 0
check "and nothing but the report went to stdout" \
  test "$(head -1 "$raw/output.txt")" = "aadp-results 1"

"$table" --iteration "$file" >"$scratch/table.md" 2>"$scratch/table.err"
check "the table tool renders it as iteration" test $? -eq 0
check "under the iteration banner" grep -q '^\*\*ITERATION ONLY: not results.\*\*' "$scratch/table.md"
check "with the speed table" grep -q 'QuantLib against this project on one thread' "$scratch/table.md"
"$table" "$file" >/dev/null 2>"$scratch/table-plain.err"
check "the table tool without --iteration refuses it" test $? -ne 0
check "and says why" grep -q 'not reportable: iteration' "$scratch/table-plain.err"

# The run flags belong to the report file's header; the text has none.
"$binary" speed --scale 64 --repetitions 1 --reportable >/dev/null 2>"$scratch/flags.err"
check "the text refuses a run flag" test $? -eq 2
"$binary" speed --report --scale 64 --out "$scratch/x.txt" >/dev/null 2>"$scratch/out.err"
check "a report refuses --out" test $? -eq 2
"$binary" smoke --report >/dev/null 2>"$scratch/smoke.err"
check "only the speed run writes a report" test $? -eq 2
"$binary" speed --report --scale 3 >/dev/null 2>"$scratch/scale.err"
check "a scale that is not a power of two is refused" test $? -eq 2

if ((failures)); then
  echo "$failures check(s) failed"
  exit 1
fi
echo "all QuantLib speed report script checks passed"
