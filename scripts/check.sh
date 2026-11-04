#!/usr/bin/env bash
# Runs every local check, one after another, and says what happened to each.
#
#   scripts/check.sh [--only <step,...>] [--allow-skip-step <step,...>]
#                    [--allow-skipped-tests <regex>]
#   scripts/check.sh --list
#
# Every step runs, whatever happened to the ones before it. Each prints one PASS, FAIL or SKIP
# line and writes build/logs/check/check-<step>.log. Steps run strictly in sequence, so only
# one sanitizer build or tidy run exists at a time.
#
# The last step, fault-injection, runs every fault in faults/manifest.json against the committed
# HEAD, in a copy under build/fault, so uncommitted edits have no effect on it. Faults that test
# this script run it in that copy with --only, so the step never runs itself.
#
# A required step that could not run, because a tool or file is missing, is a FAIL. So is any
# test that ctest did not run. Two named flags relax that, and whatever they allow is printed
# and counted in the total:
#   --allow-skip-step <step,...>    these steps may report SKIP when they cannot run
#   --allow-skipped-tests <regex>   skipped tests with a matching name do not fail their step
#
# --only is for development. It runs the named steps and always exits non-zero, so a partial
# run can never read as a pass.
#
# Environment:
#   AADP_JOBS          build, test and tidy jobs, default 6. More than 6 is refused.
#   AADP_FUZZ_SECONDS  seconds per fuzz target, default 60.
#   CC, CXX, SDKROOT, PATH  the toolchain. This script sets none of them.
#
# Exit codes: 0 every step passed, 1 a step failed, 2 usage error, 3 partial run with no failure.
#
# Runs on the bash 3.2 that macOS ships.
set -uo pipefail

readonly MAX_JOBS=6
readonly COULD_NOT_RUN=77
readonly ALL_STEPS='nice format tidy repo-scan release debug asan-ubsan tsan fuzz measured-plan
  data-verify actionlint fault-injection'

usage() {
  printf 'usage: check.sh [--only <step,...>] [--allow-skip-step <step,...>]\n' >&2
  printf '                [--allow-skipped-tests <regex>] | --list\n' >&2
  exit 2
}

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd -- "$repo_root" || exit 2

is_step() {
  local step
  for step in $ALL_STEPS; do
    [[ $step == "$1" ]] && return 0
  done
  return 1
}

# Prints the comma list one per line, after checking each name.
split_steps() {
  local item
  [[ -n $1 ]] || usage
  for item in ${1//,/ }; do
    if ! is_step "$item"; then
      printf 'check: unknown step %s. Steps: %s\n' "$item" "$(printf '%s ' $ALL_STEPS)" >&2
      return 1
    fi
    printf '%s\n' "$item"
  done
}

only=
allow_steps=
allow_tests=
partial=0
while (($# > 0)); do
  case $1 in
    --only)
      (($# >= 2)) || usage
      only=$(split_steps "$2") || exit 2
      partial=1
      shift 2
      ;;
    --allow-skip-step)
      (($# >= 2)) || usage
      allow_steps=$(split_steps "$2") || exit 2
      shift 2
      ;;
    --allow-skipped-tests)
      (($# >= 2)) || usage
      [[ -n $2 ]] || usage
      allow_tests=$2
      shift 2
      ;;
    --list)
      printf '%s\n' $ALL_STEPS
      exit 0
      ;;
    *) usage ;;
  esac
done

jobs=${AADP_JOBS:-6}
if [[ ! $jobs =~ ^[1-9][0-9]*$ ]] || ((jobs > MAX_JOBS)); then
  printf 'check: AADP_JOBS must be a whole number from 1 to %d, not %s\n' "$MAX_JOBS" "$jobs" >&2
  exit 2
fi
fuzz_seconds=${AADP_FUZZ_SECONDS:-60}
if [[ ! $fuzz_seconds =~ ^[1-9][0-9]*$ ]]; then
  printf 'check: AADP_FUZZ_SECONDS must be a positive whole number, not %s\n' "$fuzz_seconds" >&2
  exit 2
fi

log_dir=build/logs/check
mkdir -p -- "$log_dir" || exit 2

# A tool's exit 2 means it could not run. The caller of a step tells that from a failure.
tool() {
  "$@"
  local code=$?
  if ((code == 2)); then
    return "$COULD_NOT_RUN"
  fi
  return "$code"
}

step_nice() {
  local level
  level=$(ps -o ni= -p $$) || return "$COULD_NOT_RUN"
  level=${level//[[:space:]]/}
  if [[ -z $level ]]; then
    printf 'cannot read the nice level\n'
    return "$COULD_NOT_RUN"
  fi
  if [[ $level != 0 ]]; then
    printf 'nice level is %s and must be 0: the stamp tool refuses anything else, and tests of it\n' \
      "$level"
    printf 'fail. A bare & in zsh does this through BG_NICE. Run check.sh in the foreground.\n'
    return 1
  fi
  printf 'nice level 0\n'
}

step_preset() {
  local preset=$1 allow=()
  if [[ -n $allow_tests ]]; then
    allow=(--allow-skipped "$allow_tests")
  fi
  command -v cmake >/dev/null 2>&1 || return "$COULD_NOT_RUN"
  cmake --preset "$preset" || return 1
  cmake --build --preset "$preset" --parallel "$jobs" || return 1
  tool python3 -I scripts/run_tests.py ctest "$preset" --jobs "$jobs" \
    --junit "$log_dir/check-$preset-junit.xml" ${allow[@]+"${allow[@]}"}
}

step_fuzz() {
  command -v cmake >/dev/null 2>&1 || return "$COULD_NOT_RUN"
  cmake --preset fuzz || return 1
  cmake --build --preset fuzz --parallel "$jobs" || return 1
  tool python3 -I scripts/run_tests.py fuzz --seconds "$fuzz_seconds"
}

# The committed rows must come back byte for byte. aadp_thread_invariance_test shows that the
# thread count changes no bit of a result, so this can honour the job cap.
step_measured_plan() {
  local output=$log_dir/check-measured-plan-output.txt
  command -v cmake >/dev/null 2>&1 || return "$COULD_NOT_RUN"
  rm -f -- "$output"
  cmake --preset release || return 1
  cmake --build --preset release --parallel "$jobs" --target aadp_run_oracles || return 1
  build/release/bin/aadp_run_oracles run --plan references/comparison_plan.csv \
    --references references/oracle_values.csv --threads "$jobs" >"$output" || return 1
  tool python3 -I scripts/compare_measured.py "$output" references/measured_results.csv
}

step_data_verify() {
  scripts/fetch_data.sh --verify
  local code=$?
  if ((code == 4)); then
    return "$COULD_NOT_RUN"
  fi
  return "$code"
}

step_actionlint() {
  scripts/fetch_tools.sh || return "$COULD_NOT_RUN"
  build/tools/actionlint -no-color -shellcheck build/tools/shellcheck .github/workflows/ci.yml
  local code=$?
  if ((code == 0)); then
    printf 'actionlint: no findings\n'
  fi
  return "$code"
}

step_fault_injection() {
  local output=$log_dir/check-fault-injection-results.txt
  rm -f -- "$output"
  tool python3 -I scripts/fault_injection.py --jobs "$jobs" --output "$output"
}

run_body() {
  case $1 in
    nice) step_nice ;;
    format) tool scripts/check_format.sh ;;
    tidy) tool scripts/run_clang_tidy.sh --jobs "$jobs" release fuzz ;;
    repo-scan) tool python3 -I scripts/scan_repo.py ;;
    release | debug | asan-ubsan | tsan) step_preset "$1" ;;
    fuzz) step_fuzz ;;
    measured-plan) step_measured_plan ;;
    data-verify) step_data_verify ;;
    actionlint) step_actionlint ;;
    fault-injection) step_fault_injection ;;
    *)
      printf 'no body for step %s\n' "$1"
      return 1
      ;;
  esac
}

listed() {
  printf '%s\n' "$2" | grep -qxF -- "$1"
}

report() {
  printf '%-4s  %-15s %5ss  %s\n' "$1" "$2" "$3" "$4" | tee -a "$summary"
}

summary=$log_dir/check-summary.txt
: >"$summary" || exit 2

printf 'check: jobs=%s fuzz_seconds=%s\n' "$jobs" "$fuzz_seconds" | tee -a "$summary"
if [[ -n $allow_steps ]]; then
  printf 'check: FLAG --allow-skip-step: %s\n' "$(printf '%s ' $allow_steps)" | tee -a "$summary"
fi
if [[ -n $allow_tests ]]; then
  printf 'check: FLAG --allow-skipped-tests: %s\n' "$allow_tests" | tee -a "$summary"
fi
if ((partial != 0)); then
  printf 'check: PARTIAL RUN: --only %s\n' "$(printf '%s ' $only)" | tee -a "$summary"
fi

passed=0
failed=0
skipped_steps=0
skipped_tests=0
total=0
for step in $ALL_STEPS; do
  total=$((total + 1))
  log=$log_dir/check-$step.log
  if ((partial != 0)) && ! listed "$step" "$only"; then
    skipped_steps=$((skipped_steps + 1))
    report SKIP "$step" 0 'not selected by --only'
    continue
  fi

  started=$SECONDS
  (run_body "$step") >"$log" 2>&1
  code=$?
  elapsed=$((SECONDS - started))

  # The counts line from run_tests.py, for steps that ran ctest.
  counts=$(grep -E '^run_tests: [a-z0-9-]+ total=[0-9]+ passed=' -- "$log" | tail -n 1)
  note=$log
  if [[ -n $counts ]]; then
    note="$log  ${counts#run_tests: }"
    step_skips=${counts##* skipped=}
    step_skips=${step_skips%% *}
    skipped_tests=$((skipped_tests + step_skips))
  fi

  if ((code == 0)); then
    passed=$((passed + 1))
    report PASS "$step" "$elapsed" "$note"
  elif ((code == COULD_NOT_RUN)) && listed "$step" "$allow_steps"; then
    skipped_steps=$((skipped_steps + 1))
    report SKIP "$step" "$elapsed" "$note  could not run, allowed by --allow-skip-step"
  elif ((code == COULD_NOT_RUN)); then
    failed=$((failed + 1))
    report FAIL "$step" "$elapsed" "$note  required step could not run"
  else
    failed=$((failed + 1))
    last_line=$(grep -v '^[[:space:]]*$' -- "$log" | tail -n 1)
    report FAIL "$step" "$elapsed" "$log  exit $code: $last_line"
  fi
done

printf 'check: %d steps: %d passed, %d failed, %d skipped. Total skipped: %d (%d steps, %d tests)\n' \
  "$total" "$passed" "$failed" "$skipped_steps" "$((skipped_steps + skipped_tests))" \
  "$skipped_steps" "$skipped_tests" | tee -a "$summary"

if ((failed != 0)); then
  printf 'check: FAIL\n' | tee -a "$summary"
  exit 1
fi
if ((partial != 0)); then
  printf 'check: PARTIAL, NOT A PASS: only part of the checks ran\n' | tee -a "$summary"
  exit 3
fi
if ((skipped_steps != 0 || skipped_tests != 0)); then
  printf 'check: PASS, with the skips above allowed by the flags above\n' | tee -a "$summary"
  exit 0
fi
printf 'check: PASS\n' | tee -a "$summary"
