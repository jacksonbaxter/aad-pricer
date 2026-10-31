#!/usr/bin/env bash
# Offline test of scripts/fetch_data.sh. Temporary manifests with file:// URLs stand
# in for the network, under the scratch directory given as the only argument.
set -uo pipefail

if (($# != 1)); then
  printf 'usage: fetch_data_test.sh <scratch-dir>\n' >&2
  exit 1
fi
scratch=$1
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
fetch=$repo_root/scripts/fetch_data.sh

rm -rf -- "$scratch"
mkdir -p -- "$scratch" || exit 1
scratch=$(cd -- "$scratch" && pwd -P)

failures=0
check() {
  local description=$1
  shift
  if "$@"; then
    printf 'ok    %s\n' "$description"
  else
    printf 'FAIL  %s\n' "$description"
    failures=$((failures + 1))
  fi
}

# run_fetch <exit-var> <output-file> <args...>
run_fetch() {
  local exit_var=$1 output=$2
  shift 2
  "$fetch" "$@" >"$output" 2>&1
  printf -v "$exit_var" '%s' "$?"
}

equal() { [[ $1 == "$2" ]]; }
contains() { grep -qF -- "$2" "$1"; }
no_temp_left() { [[ -z $(find "$1" -name '.fetch.*' -print 2>/dev/null) ]]; }

if command -v sha256sum >/dev/null 2>&1; then
  hash_of() { sha256sum <"$1" | cut -d' ' -f1; }
else
  hash_of() { shasum -a 256 <"$1" | cut -d' ' -f1; }
fi

source_dir=$scratch/source
mkdir -p "$source_dir"
# CRLF, a lone CR, cp950 pairs, a NUL and no final newline: anything that rewrites
# text would change these bytes.
printf 'made,up\r\n\xa4\x40\xaf\xeb,bare\rcr\n\0tail' >"$source_dir/sample.csv"
good_hash=$(hash_of "$source_dir/sample.csv")
wrong_hash=0000000000000000000000000000000000000000000000000000000000000000

manifest() { printf 'sample.csv file://%s %s\n' "$1" "$2"; }

# --- fine: downloads, keeps the bytes, leaves no temp file
good=$scratch/good.txt
manifest "$source_dir/sample.csv" "$good_hash" >"$good"
dir=$scratch/ok
run_fetch code "$scratch/ok.out" --manifest "$good" --data-dir "$dir"
check 'good capture exits 0' equal "$code" 0
check 'good capture is byte-for-byte' cmp "$source_dir/sample.csv" "$dir/sample.csv"
check 'good capture leaves no temp file' no_temp_left "$dir"

run_fetch code "$scratch/verify.out" --manifest "$good" --data-dir "$dir" --verify
check '--verify on a good file exits 0' equal "$code" 0

# Present and correct: the script must not need the source again.
mv "$source_dir/sample.csv" "$source_dir/sample.moved"
run_fetch code "$scratch/cached.out" --manifest "$good" --data-dir "$dir"
check 'verified file is not downloaded again' equal "$code" 0
check '--verify does not touch the network' equal "$(
  "$fetch" --manifest "$good" --data-dir "$dir" --verify >/dev/null 2>&1
  printf '%s' "$?"
)" 0
mv "$source_dir/sample.moved" "$source_dir/sample.csv"

# --- checksum mismatch: nothing lands under the real name
bad=$scratch/bad.txt
manifest "$source_dir/sample.csv" "$wrong_hash" >"$bad"
dir=$scratch/mismatch
run_fetch code "$scratch/mismatch.out" --manifest "$bad" --data-dir "$dir"
check 'wrong hash exits 2' equal "$code" 2
check 'wrong hash names the file' contains "$scratch/mismatch.out" 'checksum mismatch: sample.csv'
check 'wrong hash says it was discarded' contains "$scratch/mismatch.out" \
  'The download was discarded.'
check 'wrong hash lands no file' test ! -e "$dir/sample.csv"
check 'wrong hash leaves no temp file' no_temp_left "$dir"

# --verify on a corrupted file, and a fetch over one, both refuse.
dir=$scratch/corrupt
mkdir -p "$dir"
printf 'damaged' >"$dir/sample.csv"
run_fetch code "$scratch/corrupt.out" --manifest "$good" --data-dir "$dir" --verify
check '--verify on a damaged file exits 2' equal "$code" 2
check '--verify names the damaged file' contains "$scratch/corrupt.out" \
  "checksum mismatch: $dir/sample.csv"
run_fetch code "$scratch/corrupt2.out" --manifest "$good" --data-dir "$dir"
check 'fetch over a damaged file exits 2' equal "$code" 2
check 'fetch names the damaged file' contains "$scratch/corrupt2.out" \
  "checksum mismatch: $dir/sample.csv"
check 'fetch says the file was left alone' contains "$scratch/corrupt2.out" \
  'The existing file was not touched.'
check 'fetch does not overwrite a damaged file' equal "$(cat "$dir/sample.csv")" damaged

# --- capture unavailable
gone=$scratch/gone.txt
manifest "$source_dir/no-such-file.csv" "$good_hash" >"$gone"
dir=$scratch/gone
run_fetch code "$scratch/gone.out" --manifest "$gone" --data-dir "$dir"
check 'missing capture exits 3' equal "$code" 3
check 'missing capture lands no file' test ! -e "$dir/sample.csv"
check 'missing capture leaves no temp file' no_temp_left "$dir"
check 'message says the capture is gone' contains "$scratch/gone.out" 'capture is gone'
check 'message says to report it' contains "$scratch/gone.out" 'Report this to the user'
check 'message forbids substitution' contains "$scratch/gone.out" 'Do not substitute any other source'
check 'message says there is no fallback' contains "$scratch/gone.out" 'There is no fallback URL'

# --- other usage
dir=$scratch/absent
run_fetch code "$scratch/absent.out" --manifest "$good" --data-dir "$dir" --verify
check '--verify on an absent file exits 4' equal "$code" 4
check '--verify names the absent file' contains "$scratch/absent.out" "absent: $dir/sample.csv"

climb=$scratch/climb.txt
printf '../escape.csv file://%s %s\n' "$source_dir/sample.csv" "$good_hash" >"$climb"
run_fetch code "$scratch/climb.out" --manifest "$climb" --data-dir "$scratch/climb"
check 'a name that climbs out of the data dir is refused' equal "$code" 1
check 'the refusal names the manifest line' contains "$scratch/climb.out" 'bad manifest line 1'
check 'nothing escaped the data dir' test ! -e "$scratch/escape.csv"

run_fetch code "$scratch/args.out" --no-such-option
check 'unknown option exits 1' equal "$code" 1
check 'unknown option prints the usage' contains "$scratch/args.out" 'usage: fetch_data.sh'

# --- each hash tool alone, with PATH cut down to links to what the script needs
make_path_without() {
  local excluded=$1 target=$2 tool found
  mkdir -p "$target"
  for tool in bash env curl mktemp mv rm mkdir dirname chmod sha256sum shasum; do
    if [[ $tool != "$excluded" ]] && found=$(command -v "$tool"); then
      ln -s "$found" "$target/$tool"
    fi
  done
}

for tool in sha256sum shasum; do
  other=shasum
  [[ $tool == shasum ]] && other=sha256sum
  if ! command -v "$tool" >/dev/null 2>&1; then
    printf 'SKIP  %s is not on this host, so it was not exercised\n' "$tool"
    continue
  fi
  bin=$scratch/bin-$tool
  make_path_without "$other" "$bin"
  dir=$scratch/tool-$tool
  PATH=$bin "$fetch" --manifest "$good" --data-dir "$dir" >"$scratch/$tool.out" 2>&1
  check "fetch works with $tool alone" equal "$?" 0
  check "file fetched with $tool is correct" cmp "$source_dir/sample.csv" "$dir/sample.csv"
  PATH=$bin "$fetch" --manifest "$bad" --data-dir "$dir-bad" >"$scratch/$tool-bad.out" 2>&1
  check "$tool alone still catches a mismatch" equal "$?" 2
  check "$tool alone names the mismatch" contains "$scratch/$tool-bad.out" \
    'checksum mismatch: sample.csv'
done

# --- the request itself, seen by a stand-in curl
bin=$scratch/bin-agent
mkdir -p "$bin"
make_path_without none "$bin"
rm -f "$bin/curl"
cat >"$bin/curl" <<SHIM
#!/bin/bash
printf '%s\n' "\$@" >'$scratch/curl.args'
exit 22
SHIM
chmod +x "$bin/curl"
PATH=$bin "$fetch" --manifest "$good" --data-dir "$scratch/agent" >"$scratch/agent.out" 2>&1
check 'a curl HTTP failure exits 3' equal "$?" 3
check 'a curl HTTP failure reports curl exit 22' contains "$scratch/agent.out" \
  'curl exit code: 22'
check 'User-Agent names the project' contains "$scratch/curl.args" 'aad-pricer'
agent=$(grep -A1 -F -- '--user-agent' "$scratch/curl.args" | tail -n 1)
check 'User-Agent carries a version' contains <(printf '%s' "$agent") '/1.0'
check 'request is not compressed' test -z "$(grep -F -- '--compressed' "$scratch/curl.args")"
user_name=$(id -un)
check 'User-Agent has no user name' test -z "$(printf '%s' "$agent" | grep -F -- "$user_name")"
host_name=$(hostname)
check 'User-Agent has no host name' test -z "$(printf '%s' "$agent" | grep -F -- "$host_name")"

# --- the shipped manifest
shipped=$repo_root/scripts/data_manifest.txt
check 'manifest pins the options hash' contains "$shipped" \
  fd9ddc6cafcbdd1b08cd0f80912dcbd9c19de515cdb248163183378715d53f1e
check 'manifest pins the futures hash' contains "$shipped" \
  168814667736745df0774c16e8e95b976e4b3f671e9cc3bda5e10e20ae2313b7

if ((failures != 0)); then
  printf '%d check(s) failed\n' "$failures"
  exit 1
fi
printf 'all checks passed\n'
