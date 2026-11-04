#!/usr/bin/env bash
# Installs pinned tools into build/tools/, verified by sha256, for darwin-arm64 and linux-amd64.
#
#   scripts/fetch_tools.sh [--llvm]
#
# Always installs actionlint, as build/tools/actionlint, and the shellcheck it runs on every
# run: step, as build/tools/shellcheck. Without shellcheck, actionlint skips that check without a
# word, so local and CI runs must both name this one. --llvm also installs clang, clang-format
# and clang-tidy under build/tools/llvm/, for CI: a local build takes its compiler from the
# environment. A tool already installed at the pinned version is left alone, with no download.
#
# The actionlint hashes are from the release's published checksums file. LLVM and shellcheck
# publish no such file, so their hashes are the sha256 digests GitHub reports for the release
# assets.
#
# Exit codes:
#   0  everything is installed
#   1  usage error, unsupported platform, or a required tool is missing
#   2  checksum mismatch: nothing was installed
#   3  a download failed
#   4  the installed tool does not report the pinned version
#
# Runs on the bash 3.2 that macOS ships.
set -uo pipefail

readonly EXIT_USAGE=1 EXIT_MISMATCH=2 EXIT_DOWNLOAD=3 EXIT_VERSION=4

readonly ACTIONLINT_VERSION=1.7.12
readonly ACTIONLINT_SHA256_DARWIN_ARM64=aba9ced2dee8d27fecca3dc7feb1a7f9a52caefa1eb46f3271ea66b6e0e6953f
readonly ACTIONLINT_SHA256_LINUX_AMD64=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8

readonly SHELLCHECK_VERSION=0.11.0
readonly SHELLCHECK_SHA256_DARWIN_ARM64=339b930feb1ea764467013cc1f72d09cd6b869ebf1013296ba9055ab2ffbd26f
readonly SHELLCHECK_SHA256_LINUX_AMD64=b7af85e41cc99489dcc21d66c6d5f3685138f06d34651e6d34b42ec6d54fe6f6

readonly LLVM_VERSION=23.1.4
readonly LLVM_SHA256_DARWIN_ARM64=5d3179db39adeca5b17a34d82708fe976bc60e055b05fe557878fb3fb64d39ef
readonly LLVM_SHA256_LINUX_AMD64=25d814fda50d5fb0084b42be09605d8b1595d49d5e2e03a3345d7e112ff6f9f3

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
tools_dir=$repo_root/build/tools
with_llvm=0
work_dir=

say() { printf 'fetch_tools: %s\n' "$*"; }
fail() { printf 'fetch_tools: %s\n' "$*" >&2; }

while (($# > 0)); do
  case $1 in
    --llvm)
      with_llvm=1
      shift
      ;;
    *)
      fail 'usage: fetch_tools.sh [--llvm]'
      exit "$EXIT_USAGE"
      ;;
  esac
done

platform=$(uname -s)-$(uname -m)
case $platform in
  Darwin-arm64)
    actionlint_asset=actionlint_${ACTIONLINT_VERSION}_darwin_arm64.tar.gz
    actionlint_sha256=$ACTIONLINT_SHA256_DARWIN_ARM64
    shellcheck_asset=shellcheck-v$SHELLCHECK_VERSION.darwin.aarch64.tar.gz
    shellcheck_sha256=$SHELLCHECK_SHA256_DARWIN_ARM64
    llvm_name=LLVM-$LLVM_VERSION-macOS-ARM64
    llvm_sha256=$LLVM_SHA256_DARWIN_ARM64
    ;;
  Linux-x86_64)
    actionlint_asset=actionlint_${ACTIONLINT_VERSION}_linux_amd64.tar.gz
    actionlint_sha256=$ACTIONLINT_SHA256_LINUX_AMD64
    shellcheck_asset=shellcheck-v$SHELLCHECK_VERSION.linux.x86_64.tar.gz
    shellcheck_sha256=$SHELLCHECK_SHA256_LINUX_AMD64
    llvm_name=LLVM-$LLVM_VERSION-Linux-X64
    llvm_sha256=$LLVM_SHA256_LINUX_AMD64
    ;;
  *)
    fail "no pinned tools for $platform"
    exit "$EXIT_USAGE"
    ;;
esac

for tool in curl tar; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    fail "$tool is not available"
    exit "$EXIT_USAGE"
  fi
done
if command -v sha256sum >/dev/null 2>&1; then
  sha256_stdin() { sha256sum; }
elif command -v shasum >/dev/null 2>&1; then
  sha256_stdin() { shasum -a 256; }
else
  fail 'neither sha256sum nor shasum is available'
  exit "$EXIT_USAGE"
fi

cleanup() {
  if [[ -n $work_dir ]]; then
    rm -rf -- "$work_dir"
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p -- "$tools_dir" || exit "$EXIT_USAGE"
work_dir=$(mktemp -d "$tools_dir/.fetch.XXXXXX") || {
  fail "cannot create a temporary directory in $tools_dir"
  exit "$EXIT_USAGE"
}

# Downloads into the work directory. Nothing from the archive is opened before this returns.
fetch_verified() {
  local url=$1 expected=$2 path=$3 digest actual
  say "downloading $url"
  if ! curl -q --silent --show-error --fail --location --proto '=https' --tlsv1.2 \
    --retry 3 --retry-delay 5 --connect-timeout 30 --output "$path" --url "$url"; then
    fail "download failed: $url"
    exit "$EXIT_DOWNLOAD"
  fi
  digest=$(sha256_stdin <"$path") || exit "$EXIT_USAGE"
  actual=${digest%% *}
  if [[ $actual != "$expected" ]]; then
    fail "checksum mismatch, refusing: $url"
    fail "  expected $expected"
    fail "  actual   $actual"
    exit "$EXIT_MISMATCH"
  fi
  say "sha256 ok $actual"
}

install_actionlint() {
  local binary=$tools_dir/actionlint archive version
  version=$("$binary" -version 2>/dev/null) && [[ ${version%%$'\n'*} == "$ACTIONLINT_VERSION" ]] && {
    say "actionlint $ACTIONLINT_VERSION already installed"
    return 0
  }
  archive=$work_dir/$actionlint_asset
  fetch_verified \
    "https://github.com/rhysd/actionlint/releases/download/v$ACTIONLINT_VERSION/$actionlint_asset" \
    "$actionlint_sha256" "$archive"
  mkdir -p -- "$work_dir/actionlint" || exit "$EXIT_USAGE"
  tar -xzf "$archive" -C "$work_dir/actionlint" actionlint || exit "$EXIT_USAGE"
  version=$("$work_dir/actionlint/actionlint" -version) || exit "$EXIT_VERSION"
  if [[ ${version%%$'\n'*} != "$ACTIONLINT_VERSION" ]]; then
    fail "the archive holds actionlint ${version%%$'\n'*}, not $ACTIONLINT_VERSION"
    exit "$EXIT_VERSION"
  fi
  mv -f -- "$work_dir/actionlint/actionlint" "$binary" || exit "$EXIT_USAGE"
  say "installed actionlint $ACTIONLINT_VERSION at build/tools/actionlint"
}

shellcheck_version_ok() {
  local output
  output=$("$1" --version 2>/dev/null) || return 1
  [[ $output == *$'\n'"version: $SHELLCHECK_VERSION"$'\n'* ]]
}

install_shellcheck() {
  local binary=$tools_dir/shellcheck archive
  if shellcheck_version_ok "$binary"; then
    say "shellcheck $SHELLCHECK_VERSION already installed"
    return 0
  fi
  archive=$work_dir/$shellcheck_asset
  fetch_verified \
    "https://github.com/koalaman/shellcheck/releases/download/v$SHELLCHECK_VERSION/$shellcheck_asset" \
    "$shellcheck_sha256" "$archive"
  mkdir -p -- "$work_dir/shellcheck" || exit "$EXIT_USAGE"
  tar -xzf "$archive" -C "$work_dir/shellcheck" --strip-components=1 \
    "shellcheck-v$SHELLCHECK_VERSION/shellcheck" || exit "$EXIT_USAGE"
  if ! shellcheck_version_ok "$work_dir/shellcheck/shellcheck"; then
    fail "the archive does not hold shellcheck $SHELLCHECK_VERSION"
    exit "$EXIT_VERSION"
  fi
  mv -f -- "$work_dir/shellcheck/shellcheck" "$binary" || exit "$EXIT_USAGE"
  say "installed shellcheck $SHELLCHECK_VERSION at build/tools/shellcheck"
}

llvm_version_ok() {
  local root=$1 output
  output=$("$root/bin/clang" --version 2>/dev/null) || return 1
  [[ $output == *"clang version $LLVM_VERSION"* ]] || return 1
  output=$("$root/bin/clang-format" --version 2>/dev/null) || return 1
  [[ $output == *"clang-format version $LLVM_VERSION"* ]] || return 1
  output=$("$root/bin/clang-tidy" --version 2>/dev/null) || return 1
  [[ $output == *"LLVM version $LLVM_VERSION"* ]]
}

install_llvm() {
  local root=$tools_dir/llvm archive staged file
  if llvm_version_ok "$root"; then
    say "LLVM $LLVM_VERSION already installed"
    return 0
  fi
  archive=$work_dir/$llvm_name.tar.xz
  fetch_verified \
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/$llvm_name.tar.xz" \
    "$llvm_sha256" "$archive"
  staged=$work_dir/llvm
  mkdir -p -- "$staged" || exit "$EXIT_USAGE"
  say 'extracting LLVM'
  tar -xJf "$archive" -C "$staged" --strip-components=1 || exit "$EXIT_USAGE"
  rm -f -- "$archive"

  # Keep what the builds, tidy and the sanitizers use. The rest is gigabytes of other tools and
  # of static compiler libraries. lib/clang holds the builtin headers and sanitizer runtimes.
  for file in "$staged"/bin/*; do
    case ${file##*/} in
      clang | clang++ | clang-[0-9]* | clang-format | clang-tidy) ;;
      lld | ld.lld | ld64.lld | llvm-ar | llvm-nm | llvm-ranlib | llvm-symbolizer) ;;
      *) rm -f -- "$file" ;;
    esac
  done
  find "$staged/lib" -maxdepth 1 -name '*.a' ! -name 'libc++*' ! -name 'libunwind*' -delete ||
    exit "$EXIT_USAGE"

  if ! llvm_version_ok "$staged"; then
    fail "the extracted tree does not report LLVM $LLVM_VERSION"
    exit "$EXIT_VERSION"
  fi
  rm -rf -- "$root"
  mv -- "$staged" "$root" || exit "$EXIT_USAGE"
  say "installed LLVM $LLVM_VERSION at build/tools/llvm"
}

install_actionlint
install_shellcheck
if ((with_llvm != 0)); then
  install_llvm
fi
