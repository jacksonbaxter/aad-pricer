#!/usr/bin/env bash
# Configures, builds and tests each commit of a range, without touching the working tree.
#
#   scripts/verify_commits.sh [--jobs <n>] [--allow-skipped <regex>] <range>
#
# <range> goes to git rev-list, for example main..jb-build or HEAD~3..HEAD. Commits run oldest
# first. Each is exported with git archive into build/commit-check/src. Only files that changed
# are replaced there, by scripts/sync_tree.py, so build/commit-check/build and its FetchContent
# downloads carry over from one commit to the next. Real-file tests see the existing data/
# through a link. This reads history and never rewrites it. No commit is ever checked out.
#
# build/logs/commits/summary.txt gets one line per commit as it finishes:
#   <hash> | <subject> | configure=<r> | build=<r> | tests=<r> | verdict=<pass|fail|no-build>
# with r one of ok, fail or -, and a tests=ok(...) count. A commit from before CMake exists is
# no-build. A failure never stops the run. Each step's log is <hash>-<step>.log in the same
# directory. A skipped test is a failed commit unless --allow-skipped names it.
#
# Jobs come from --jobs, then AADP_JOBS, and default to 6. The toolchain comes from CC, CXX,
# SDKROOT and PATH.
#
# Exit codes: 0 no commit failed, 1 a commit failed, 2 usage error or could not start.
#
# Runs on the bash 3.2 that macOS ships.
set -uo pipefail

usage() {
  printf 'usage: verify_commits.sh [--jobs <n>] [--allow-skipped <regex>] <range>\n' >&2
  exit 2
}

jobs=${AADP_JOBS:-6}
allow=
range=
while (($# > 0)); do
  case $1 in
    --jobs)
      (($# >= 2)) || usage
      jobs=$2
      shift 2
      ;;
    --allow-skipped)
      (($# >= 2)) || usage
      allow=$2
      shift 2
      ;;
    -*) usage ;;
    *)
      [[ -z $range ]] || usage
      range=$1
      shift
      ;;
  esac
done
[[ -n $range && $jobs =~ ^[1-9][0-9]*$ ]] || usage

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd -- "$repo_root" || exit 2

for tool in cmake ctest git ninja python3 tar; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'verify_commits: %s is not available\n' "$tool" >&2
    exit 2
  fi
done
level=$(ps -o ni= -p $$)
level=${level//[[:space:]]/}
if [[ $level != 0 ]]; then
  printf 'verify_commits: nice level is %s and must be 0, or the stamping tests fail\n' "$level" >&2
  exit 2
fi

commits=$(git rev-list --reverse "$range") || exit 2
if [[ -z $commits ]]; then
  printf 'verify_commits: the range %s has no commits\n' "$range" >&2
  exit 2
fi

check_dir=$repo_root/build/commit-check
source_dir=$check_dir/src
export_dir=$check_dir/export
build_dir=$check_dir/build
deps_dir=$check_dir/deps
log_dir=$repo_root/build/logs/commits
summary=$log_dir/summary.txt
mkdir -p -- "$source_dir" "$build_dir" "$deps_dir" "$log_dir" || exit 2
: >"$summary" || exit 2

# provenance.cpp reads the commit from this file in a tree with no .git, and git commands in
# tests must not walk up from the export into this repository.
export GIT_CEILING_DIRECTORIES=$check_dir

# Replaces only what differs, so unchanged files keep their times and ninja rebuilds no more
# than the commit changed. data is the link to the real captures, and is never part of a commit.
# scripts/sync_tree.py does what rsync -rlc --delete would, with no rsync on the host; the
# script comes from this tree, not from the commit being checked.
export_commit() {
  local commit=$1
  rm -rf -- "$export_dir"
  mkdir -p -- "$export_dir" || return 1
  git archive --format=tar "$commit" | tar -x -C "$export_dir" || return 1
  printf '%s\n' "$commit" >"$export_dir/COMMIT" || return 1
  python3 -I "$repo_root/scripts/sync_tree.py" --keep data "$export_dir" "$source_dir" ||
    return 1
  rm -rf -- "$export_dir"
  if [[ ! -L $source_dir/data ]]; then
    rm -rf -- "$source_dir/data"
    ln -s "$repo_root/data" "$source_dir/data" || return 1
  fi
}

any_failed=0
record() {
  printf '%s | %s | configure=%s | build=%s | tests=%s | verdict=%s\n' \
    "$short" "$subject" "$1" "$2" "$3" "$4" | tee -a "$summary"
  if [[ $4 == fail ]]; then
    any_failed=1
  fi
}

allow_arguments=()
if [[ -n $allow ]]; then
  allow_arguments=(--allow-skipped "$allow")
fi

for commit in $commits; do
  short=$(git rev-parse --short "$commit")
  subject=$(git log -1 --format=%s "$commit" | tr '|' '/')

  if ! git cat-file -e "$commit:CMakeLists.txt" 2>/dev/null; then
    record - - - no-build
    continue
  fi
  if ! export_commit "$commit" >"$log_dir/$short-export.log" 2>&1; then
    record - - - fail
    continue
  fi

  if ! (cd -- "$source_dir" && cmake --preset release -B "$build_dir" \
    "-DFETCHCONTENT_BASE_DIR=$deps_dir") >"$log_dir/$short-configure.log" 2>&1; then
    record fail - - fail
    continue
  fi
  if ! cmake --build "$build_dir" --parallel "$jobs" >"$log_dir/$short-build.log" 2>&1; then
    record ok fail - fail
    continue
  fi

  test_log=$log_dir/$short-test.log
  junit=$log_dir/$short-junit.xml
  rm -f -- "$junit"
  (cd -- "$source_dir" && ctest --test-dir "$build_dir" --parallel "$jobs" --no-tests=error \
    --output-on-failure --output-junit "$junit" --test-output-size-passed 10000000) \
    >"$test_log" 2>&1
  ctest_code=$?
  python3 -I scripts/run_tests.py verdict "$junit" ${allow_arguments[@]+"${allow_arguments[@]}"} \
    >>"$test_log" 2>&1
  verdict_code=$?
  counts=$(grep -E '^run_tests: tests total=' -- "$test_log" | tail -n 1)
  counts=${counts#run_tests: tests }
  counts=${counts// /,}
  if ((ctest_code == 0 && verdict_code == 0)); then
    record ok ok "ok($counts)" pass
  else
    record ok ok "fail($counts)" fail
  fi
done

printf 'verify_commits: summary in build/logs/commits/summary.txt\n'
exit "$any_failed"
