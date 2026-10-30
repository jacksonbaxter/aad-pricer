#!/usr/bin/env bash
# Downloads the pinned captures named in the manifest and verifies their sha256.
#
#   scripts/fetch_data.sh [--data-dir <dir>] [--manifest <file>] [--verify]
#
# --verify checks the local files and never touches the network. A file that is
# already present and correct is not downloaded again.
#
# Exit codes:
#   0  every file is present and verified
#   1  usage error, bad manifest, or a required tool is missing
#   2  checksum mismatch
#   3  a capture is unavailable
#   4  --verify found a file absent
#
# Runs on the bash 3.2 that macOS ships.
set -uo pipefail

readonly EXIT_USAGE=1 EXIT_MISMATCH=2 EXIT_UNAVAILABLE=3 EXIT_ABSENT=4
# Internet Archive's developer guide requires the tool name and version on
# automated requests. Nothing personal belongs here.
readonly USER_AGENT='aad-pricer-fetch-data/1.0 (aad-pricer option pricing project; pinned data capture)'

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
data_dir=$(dirname -- "$script_dir")/data
manifest=$script_dir/data_manifest.txt
verify_only=0
tmp_file=

say() { printf 'fetch_data: %s\n' "$*"; }
fail() { printf 'fetch_data: %s\n' "$*" >&2; }

usage() {
  fail 'usage: fetch_data.sh [--data-dir <dir>] [--manifest <file>] [--verify]'
  exit "$EXIT_USAGE"
}

while (($# > 0)); do
  case $1 in
    --data-dir)
      (($# >= 2)) || usage
      data_dir=$2
      shift 2
      ;;
    --manifest)
      (($# >= 2)) || usage
      manifest=$2
      shift 2
      ;;
    --verify)
      verify_only=1
      shift
      ;;
    *) usage ;;
  esac
done

# Reads stdin rather than a path argument, the one interface the two tools share.
if command -v sha256sum >/dev/null 2>&1; then
  sha256_stdin() { sha256sum; }
elif command -v shasum >/dev/null 2>&1; then
  sha256_stdin() { shasum -a 256; }
else
  fail 'neither sha256sum nor shasum is available'
  exit "$EXIT_USAGE"
fi

sha256_of() {
  local digest
  digest=$(sha256_stdin <"$1") || return 1
  printf '%s' "${digest%% *}"
}

report_mismatch() {
  fail "checksum mismatch: $1"
  fail "  expected $2"
  fail "  actual   $3"
}

report_unavailable() {
  fail "capture unavailable: $1"
  fail "  URL: $2"
  fail "  curl exit code: $3"
  fail 'This capture is gone or cannot be reached. Report this to the user.'
  fail 'Do not substitute any other source. There is no fallback URL.'
}

names=()
urls=()
hashes=()

load_manifest() {
  local name url hash extra line_no=0
  if [[ ! -r $manifest ]]; then
    fail "cannot read manifest: $manifest"
    exit "$EXIT_USAGE"
  fi
  while IFS=$' \t' read -r name url hash extra || [[ -n ${name:-} ]]; do
    line_no=$((line_no + 1))
    if [[ -z ${name:-} || $name == \#* ]]; then
      continue
    fi
    # The name becomes a path under the data directory, so it may not climb out.
    if [[ ! $name =~ ^[A-Za-z0-9_][A-Za-z0-9._-]*$ || -z ${url:-} || -n ${extra:-} ||
      ! ${hash:-} =~ ^[0-9a-f]{64}$ ]]; then
      fail "bad manifest line $line_no in $manifest"
      exit "$EXIT_USAGE"
    fi
    names+=("$name")
    urls+=("$url")
    hashes+=("$hash")
  done <"$manifest"
  if ((${#names[@]} == 0)); then
    fail "manifest has no entries: $manifest"
    exit "$EXIT_USAGE"
  fi
}

verify_all() {
  local i name path actual status=0 absent=0
  for ((i = 0; i < ${#names[@]}; i++)); do
    name=${names[i]}
    path=$data_dir/$name
    if [[ ! -f $path ]]; then
      fail "absent: $path"
      absent=1
      continue
    fi
    actual=$(sha256_of "$path") || exit "$EXIT_USAGE"
    if [[ $actual == "${hashes[i]}" ]]; then
      say "ok $name $actual"
    else
      report_mismatch "$path" "${hashes[i]}" "$actual"
      status=$EXIT_MISMATCH
    fi
  done
  if ((status != 0)); then
    return "$status"
  fi
  if ((absent != 0)); then
    return "$EXIT_ABSENT"
  fi
  return 0
}

download() {
  # -q first, so a local .curlrc cannot change the request. No --compressed: the
  # bytes on disk must be the bytes served. file is here for the offline test.
  curl -q --silent --show-error --fail --location \
    --proto '=https,file' --proto-redir '=https' \
    --retry 3 --retry-delay 5 --connect-timeout 30 --max-time 300 \
    --user-agent "$USER_AGENT" --output "$2" --url "$1"
}

fetch_one() {
  local name=$1 url=$2 expected=$3 dest=$data_dir/$1 actual code
  if [[ -e $dest ]]; then
    actual=$(sha256_of "$dest") || exit "$EXIT_USAGE"
    if [[ $actual == "$expected" ]]; then
      say "already present $name $actual"
      return 0
    fi
    # Left alone: overwriting it could hide whatever damaged it.
    report_mismatch "$dest" "$expected" "$actual"
    fail 'The existing file was not touched. Move it away and run this again.'
    exit "$EXIT_MISMATCH"
  fi

  # Same directory as the destination, so the final rename is atomic.
  tmp_file=$(mktemp "$data_dir/.fetch.XXXXXX") || {
    fail "cannot create a temporary file in $data_dir"
    exit "$EXIT_USAGE"
  }
  download "$url" "$tmp_file"
  code=$?
  if ((code != 0)); then
    report_unavailable "$name" "$url" "$code"
    exit "$EXIT_UNAVAILABLE"
  fi
  actual=$(sha256_of "$tmp_file") || exit "$EXIT_USAGE"
  if [[ $actual != "$expected" ]]; then
    report_mismatch "$name" "$expected" "$actual"
    fail 'The download was discarded. Do not substitute any other source.'
    exit "$EXIT_MISMATCH"
  fi
  chmod 0644 "$tmp_file" || exit "$EXIT_USAGE"
  mv -f -- "$tmp_file" "$dest" || exit "$EXIT_USAGE"
  tmp_file=
  say "fetched $name $actual"
}

cleanup() {
  if [[ -n $tmp_file ]]; then
    rm -f -- "$tmp_file"
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

load_manifest
if ((verify_only != 0)); then
  verify_all
  exit $?
fi

if ! command -v curl >/dev/null 2>&1; then
  fail 'curl is not available'
  exit "$EXIT_USAGE"
fi
mkdir -p -- "$data_dir" || exit "$EXIT_USAGE"
for ((i = 0; i < ${#names[@]}; i++)); do
  fetch_one "${names[i]}" "${urls[i]}" "${hashes[i]}"
done
