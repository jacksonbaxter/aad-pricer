#!/usr/bin/env bash
# aadp_metal_report's table (GPU-2, GPU-3) and timing (GPU-1) as report files through
# scripts/run_bench.sh, against a stand-in measure.sh, at a reduced size: each is stamped with the
# run header, its tables hold every figure of the text bit for bit, it says it holds prices only,
# the table tool renders it, and the run flags belong to the report alone. Needs a Metal device;
# without one it skips with the reason (exit 77), as the Gpu tests do.
# Usage: run_bench_metal_test.sh <aadp_metal_report> <aadp_metal_info> <scratch dir>
set -uo pipefail

binary=$1
info=$2
scratch=$3
script=scripts/run_bench.sh
failures=0

line=$("$info")
if [[ $line == "metal: none"* ]]; then
  echo "SKIP no Metal device: $line"
  exit 77
fi

# The stamp refuses a niced run, so nothing below can pass from a niced shell.
niceness=$(ps -o ni= -p $$ | tr -d ' ')
if [[ $niceness != 0 ]]; then
  echo "FAILED: run_bench_metal_test needs nice 0, it is at nice ${niceness:-unknown}:" \
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

# run <name> <args...>: one iteration run through run_bench.sh; sets file and raw.
run() {
  local name=$1
  shift
  AADP_NOT_REPORTABLE=iteration AADP_MEASURE=$scratch/measure_ok.sh \
    "$script" "$binary" "$name" "$@" >/dev/null 2>"$scratch/$name.err"
  check "$name: an iteration run completes and is stamped" test $? -eq 0
  file=$(ls -t "$AADP_RESULTS_DIR"/"$name"-*.txt 2>/dev/null | head -1)
  check "$name: a stamped file is written" test -n "$file"
  file=${file:-/nonexistent}
  raw=$(ls -td "$bin_dir"/../bench-runs/"$name"-* 2>/dev/null | head -1)
  raw=${raw:-/nonexistent}
  check "$name: it is not reportable" grep -q '^reportable: no$' "$file"
  check "$name: it has the run header" \
    grep -q '^commit: 0123456789abcdef0123456789abcdef01234567$' "$file"
  check "$name: its claim is a result" grep -q '^claim: result$' "$file"
  check "$name: it says it holds prices only" \
    grep -q '^report_prices_only: every figure is a price, .* AAD does not run on the GPU' "$file"
  check "$name: and names the device" grep -q '^metal_device: .' "$file"
  check "$name: with no benchmark section" test "$(grep -c '^@section benchmark_json$' "$file")" = 0
  check "$name: and no attribution, since nothing comes from the chain" \
    test "$(grep -c '^@section attribution$' "$file")" = 0
  check "$name: no table is derived from the chain" \
    test "$(grep -c '^uses_chain yes$' "$file")" = 0
  check "$name: nothing but the report went to stdout" \
    test "$(head -1 "$raw/output.txt")" = "aadp-results 1"
}

# rows <file> <section>: the section's row lines.
rows() {
  awk -v s="@section report_$2" '$0 == s { on = 1; next } /^@section / { on = 0 } on && /^row / {
    print }' "$1"
}

# matches <file> <section> <text tag> <cell columns...>: every "  = <tag> <key> v..." line of the
# text names a row of the section whose cells, at the 1-based columns given, are the same doubles.
matches() {
  local f=$1 section=$2 tag=$3
  shift 3
  local columns="$*"
  awk -v tag="$tag" -v columns="$columns" -v section="@section report_$section" '
    FNR == NR {
      if ($0 == section) { on = 1; next }
      if ($0 ~ /^@section /) on = 0
      if (on && $1 == "row") { cells[$2] = $0 }
      next
    }
    $1 == "=" && $2 == tag {
      seen++
      if (!($3 in cells)) { bad++; next }
      split(cells[$3], c, " ")
      n = split(columns, want, " ")
      for (i = 1; i <= n; i++) if (c[want[i] + 2] + 0 != $(i + 3) + 0) bad++
    }
    END { exit !(seen > 0 && bad == 0) }' "$f" "$raw/stderr.txt"
}

# The table: GPU-2's and GPU-3's figures.
run metal-table --mode=table --report --paths=65536 --threads=4
check "table: the text went to stderr" grep -q '^== the stage 9 table: N = 65536' "$raw/stderr.txt"
check "table: three tables, untimed" test "$(grep -c '^timing no$' "$file")" = 3
check "table: no timed table" test "$(grep -c '^timing yes$' "$file")" = 0
check "table: the prices name GPU-2 and GPU-3" grep -q '^targets GPU-2,GPU-3$' "$file"
check "table: four columns of three contracts" test "$(rows "$file" metal_prices | wc -l)" -eq 12
check "table: four differences of three contracts" \
  test "$(rows "$file" metal_differences | wc -l)" -eq 12
check "table: GPU-2 on three contracts" test "$(rows "$file" metal_gpu2 | wc -l)" -eq 3
check "table: each price and SE is the text's" matches "$file" metal_prices price 3 4
check "table: each difference, SE and base is the text's" \
  matches "$file" metal_differences difference 3 4 5
check "table: each GPU-2 ratio is the text's" matches "$file" metal_gpu2 gpu2 2 5
check "table: a relative difference is the difference over its base" \
  awk '$1 == "row" && $2 == "asian252.D-B" { exit !($8 + 0 == ($5 + 0) / ($7 + 0)) }
    END { }' "$file"
check "table: the verdicts are the text's" \
  test "$(rows "$file" metal_gpu2 | awk '{ print $6 }' | tr '\n' ' ')" = \
  "$(grep '^  GPU-2: ' "$raw/stderr.txt" | awk '{ print $NF }' | tr '\n' ' ')"
check "table: the reduced size is in the header" \
  grep -q '^report_tool: aadp_metal_report --mode=table --report --paths=65536 --threads=4$' "$file"
table_file=$file

# The timing: GPU-1's.
run metal-timing --mode=timing --report --paths=65536 --paths-per-buffer=16384 --reps=1 --threads=2
check "timing: the text went to stderr" grep -q '^== GPU-1, iteration only: N = 65536' \
  "$raw/stderr.txt"
check "timing: three tables, timed" test "$(grep -c '^timing yes$' "$file")" = 3
check "timing: they name GPU-1" test "$(grep -c '^targets GPU-1$' "$file")" = 3
check "timing: five lines of three contracts" test "$(rows "$file" metal_time | wc -l)" -eq 15
check "timing: the lines" test "$(rows "$file" metal_time | awk '{ print $2 }' | tr '\n' ' ')" = \
  "asian12.gpu asian12.simd-1 asian12.f32-1 asian12.simd-2 asian12.f32-2 asian252.gpu asian252.simd-1 asian252.f32-1 asian252.simd-2 asian252.f32-2 barrier156.gpu barrier156.simd-1 barrier156.f32-1 barrier156.simd-2 barrier156.f32-2 "
check "timing: each median, min and price is the text's" matches "$file" metal_time time 4 5 7
check "timing: each breakdown is the text's" matches "$file" metal_gpu_breakdown breakdown 4 5
check "timing: the setup is the text's" matches "$file" metal_setup setup 1 2 3
check "timing: four command buffers a contract" \
  test "$(rows "$file" metal_gpu_breakdown | awk '{ print $4 }' | sort -u)" = 4
check "timing: the GPU line is its own unit" \
  test "$(rows "$file" metal_time | awk '$4 == "gpu" { print $8 }' | sort -u)" = 1
check "timing: the header says what is timed" \
  grep -q '^report_timing: steady_clock; the GPU line from before buffer allocation' "$file"

"$table" --iteration "$table_file" "$file" >"$scratch/table.md" 2>"$scratch/table.err"
check "the table tool renders both as iteration" test $? -eq 0
check "under the iteration banner" grep -q '^\*\*ITERATION ONLY: not results.\*\*' "$scratch/table.md"
check "with the GPU-1 table" grep -q 'GPU-1, prices only' "$scratch/table.md"
"$table" "$file" >/dev/null 2>"$scratch/table-plain.err"
check "the table tool without --iteration refuses it" test $? -ne 0

# The run flags belong to the report file's header; the text has none, and only the table and the
# timing write a report.
"$binary" --mode=table --paths=65536 --reportable >/dev/null 2>"$scratch/flags.err"
check "the text refuses a run flag" test $? -eq 2
"$binary" --mode=properties --report >/dev/null 2>"$scratch/properties.err"
check "the properties write no report" test $? -eq 2
"$binary" --mode=batch --report >/dev/null 2>"$scratch/batch.err"
check "nor does the batch" test $? -eq 2

if ((failures)); then
  echo "$failures check(s) failed"
  exit 1
fi
echo "all Metal report script checks passed"
