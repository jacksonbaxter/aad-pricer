#!/usr/bin/env bash
# Runs clang-tidy with the repo's .clang-tidy over every project translation unit.
#
#   scripts/run_clang_tidy.sh [--jobs <n>] [<preset> ...]
#
# Each preset supplies a compile_commands.json from build/<preset>, configured here if it
# is missing. The default is release, for everything but the fuzz targets, then fuzz, for
# those. A file seen in an earlier preset is not repeated. clang-tidy comes from
# $CLANG_TIDY, or from PATH, and its findings change between releases, so a different
# version is an error. One job can pass 1 GB, so the default is 4 jobs rather than one per
# core.
#
# Exit codes: 0 clean, 1 findings, 2 could not run. Output: one summary line per file with
# findings, then every finding once, repo-relative, so a CI log says what failed by itself. The
# full text, with notes and source lines, stays under build/logs/tidy/.
set -uo pipefail

readonly PINNED_VERSION=23.1.4

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd -- "$repo_root" || exit 2

jobs=4
presets=()
while (($# > 0)); do
  case $1 in
    --jobs)
      (($# >= 2)) || {
        printf 'run_clang_tidy: --jobs needs a value\n' >&2
        exit 2
      }
      jobs=$2
      shift 2
      ;;
    -*)
      printf 'usage: run_clang_tidy.sh [--jobs <n>] [<preset> ...]\n' >&2
      exit 2
      ;;
    *)
      presets+=("$1")
      shift
      ;;
  esac
done
((${#presets[@]} > 0)) || presets=(release fuzz)

missing=()
clang_tidy=${CLANG_TIDY:-clang-tidy}
command -v "$clang_tidy" >/dev/null 2>&1 || missing+=("$clang_tidy (LLVM $PINNED_VERSION)")
command -v cmake >/dev/null 2>&1 || missing+=(cmake)
command -v python3 >/dev/null 2>&1 || missing+=(python3)
if ((${#missing[@]} > 0)); then
  printf 'run_clang_tidy: missing tool: %s\n' "${missing[@]}" >&2
  exit 2
fi
version_text=$("$clang_tidy" --version) || exit 2
if [[ $version_text != *"LLVM version $PINNED_VERSION"* ]]; then
  printf 'run_clang_tidy: need clang-tidy %s, found: %s\n' "$PINNED_VERSION" "$version_text" >&2
  exit 2
fi

log_dir=$repo_root/build/logs/tidy
rm -rf -- "$log_dir"
mkdir -p -- "$log_dir" || exit 2

# The file list comes from Python so that paths with spaces survive.
list_units() {
  python3 -I - "$1" "$repo_root" <<'PY'
import json
import os
import sys

database, root = sys.argv[1:3]
with open(database, encoding="utf-8") as file:
    entries = json.load(file)
units = set()
for entry in entries:
    path = os.path.normpath(os.path.join(entry["directory"], entry["file"]))
    relative = os.path.relpath(path, root)
    if not relative.startswith((".." + os.sep, "build" + os.sep)):
        units.add(relative)
for unit in sorted(units):
    sys.stdout.write(unit + "\0")
PY
}

# Runs one unit and keeps its output only if it has something to say.
export AADP_TIDY_BIN=$clang_tidy AADP_TIDY_LOGS=$log_dir
run_one() {
  local build_dir=$1 unit=$2 log
  log=$AADP_TIDY_LOGS/${unit//\//__}.log
  if "$AADP_TIDY_BIN" --quiet --use-color=false -p "$build_dir" "$unit" >"$log" 2>&1; then
    rm -f -- "$log"
    return 0
  fi
  printf 'run_clang_tidy: findings in %s, see %s\n' "$unit" "$log"
  return 1
}
export -f run_one

status=0
total=0
seen_file=$log_dir/.seen
: >"$seen_file"
for preset in "${presets[@]}"; do
  build_dir=build/$preset
  if [[ ! -f $build_dir/compile_commands.json ]]; then
    if ! cmake --preset "$preset" >"$log_dir/configure-$preset.log" 2>&1; then
      printf 'run_clang_tidy: configure failed, see %s\n' "$log_dir/configure-$preset.log" >&2
      exit 2
    fi
  fi
  units=()
  while IFS= read -r -d '' unit; do
    if ! grep -qxF -- "$unit" "$seen_file"; then
      units+=("$unit")
      printf '%s\n' "$unit" >>"$seen_file"
    fi
  done < <(list_units "$build_dir/compile_commands.json")
  if ((${#units[@]} == 0)); then
    continue
  fi
  total=$((total + ${#units[@]}))
  printf '%s\0' "${units[@]}" |
    xargs -0 -n 1 -P "$jobs" bash -c 'run_one "$1" "$2"' _ "$build_dir"
  code=$?
  # GNU xargs says 123 when a child fails and BSD xargs says 1, so the logs decide. A log
  # survives only for a unit that failed, and a crash inside clang-tidy leaves one too.
  if ((code != 0)); then
    if compgen -G "$log_dir/*.log" >/dev/null; then
      status=1
    else
      printf 'run_clang_tidy: xargs exited %d with no unit log\n' "$code" >&2
      exit 2
    fi
  fi
done
rm -f -- "$seen_file"

if ((total == 0)); then
  printf 'run_clang_tidy: no project translation units found\n' >&2
  exit 2
fi
if ((status != 0)); then
  # A header's finding shows up in every unit that includes it, hence sort -u. A log with no
  # diagnostic line (a crash) still fails the run, and is named above.
  findings=$(grep -h -E '(^|: )(warning|error|fatal error): ' -- "$log_dir"/*.log |
    sed "s|^$repo_root/||" | sort -u)
  if [[ -n $findings ]]; then
    printf 'run_clang_tidy: %d findings:\n%s\n' "$(printf '%s\n' "$findings" | wc -l)" "$findings"
  fi
  printf 'run_clang_tidy: FAIL, %d units checked, logs under build/logs/tidy/\n' "$total"
  exit 1
fi
printf 'run_clang_tidy: %d units clean\n' "$total"
