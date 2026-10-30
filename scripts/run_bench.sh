#!/usr/bin/env bash
# Runs one benchmark binary through measure.sh and stamps the output into a results file.
#
#   scripts/run_bench.sh <binary> <name> [args...]
#
# measure.sh is looked for in this order: $AADP_MEASURE, ../.orchestrator/measure.sh next to
# the repo, $HOME/quant-projects/bin/measure.sh. Without it the run is marked not reportable.
# Set AADP_NOT_REPORTABLE=<reason> to mark an iteration run the same way.
#
# Raw output and the stderr capture stay in the build tree, under <build>/bench-runs/. The
# stamped file goes to <build>/results/<host>/, or $AADP_RESULTS_DIR. It only reaches results/
# when somebody copies it there, after looking at it.
#
# Exit status 75 from measure.sh means retry later. It is passed through, and nothing here
# works around it. This runs on /bin/bash 3.2 on macOS.
set -uo pipefail

if (($# < 2)); then
  echo "usage: run_bench.sh <binary> <name> [args...]" >&2
  exit 64
fi
binary=$1
name=$2
shift 2

case $name in
  *[!a-z0-9-]* | "")
    echo "run_bench.sh: name must be lower-case letters, digits and hyphens" >&2
    exit 64
    ;;
esac
if [[ ! -x $binary ]]; then
  echo "run_bench.sh: cannot execute the benchmark binary" >&2
  exit 66
fi

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
bin_dir=$(cd "$(dirname "$binary")" && pwd -P)
binary_path=$bin_dir/$(basename "$binary")
build_dir=$(dirname "$bin_dir")
stamp=${AADP_STAMP:-$bin_dir/aadp_results_stamp}
if [[ ! -x $stamp ]]; then
  echo "run_bench.sh: aadp_results_stamp is not next to the binary" >&2
  exit 66
fi

host=$("$stamp" --print-host-label) || exit 1

# The header reads the commit from the source tree, and an exported tree says so itself.
source_root=${AADP_SOURCE_ROOT:-$repo}

measure=
for candidate in "${AADP_MEASURE:-}" "$repo/../.orchestrator/measure.sh" \
  "$HOME/quant-projects/bin/measure.sh"; do
  if [[ -n $candidate && -x $candidate ]]; then
    measure=$candidate
    break
  fi
done

reason=
if [[ -n ${AADP_NOT_REPORTABLE:-} ]]; then
  reason=$AADP_NOT_REPORTABLE
elif [[ -z $measure ]]; then
  reason="measure.sh not found"
elif [[ $host == other ]]; then
  reason="host other is never reportable"
fi

when=$(date -u +%Y%m%dT%H%M%SZ)
run_dir=$build_dir/bench-runs/$name-$when
results_dir=${AADP_RESULTS_DIR:-$build_dir/results/$host}
mkdir -p "$run_dir" "$results_dir" || exit 1
raw=$run_dir/output.txt
captured=$run_dir/stderr.txt
out=$results_dir/$name-$when.txt

command=("$binary_path")
if [[ $host == linux-zen1 ]]; then
  command=(taskset -c 4 "$binary_path")
fi
if [[ -n $measure ]]; then
  command=("$measure" "${command[@]}")
fi

# Run from the source root, so --source-root needs no path. Google Benchmark still sees the
# binary's full path in argv[0]; the stamp drops that.
harness_args=(--source-root=.)
stamp_args=()
if [[ -z $reason ]]; then
  harness_args+=(--reportable)
  stamp_args+=(--reportable)
  echo "run_bench.sh: $name on $host, reportable" >&2
else
  stamp_args+=("--not-reportable=$reason")
  echo "run_bench.sh: $name on $host, not reportable: $reason" >&2
fi

(cd "$source_root" && "${command[@]}" "${harness_args[@]}" "$@") >"$raw" 2>"$captured"
status=$?
if ((status == 75)); then
  echo "run_bench.sh: measure.sh says retry later (exit 75); nothing was measured" >&2
  exit 75
fi
if ((status != 0)); then
  echo "run_bench.sh: the run failed with exit $status; capture is in $captured" >&2
  exit "$status"
fi

"$stamp" --raw="$raw" --stderr="$captured" --out="$out" "${stamp_args[@]}" || exit $?
echo "$out"
