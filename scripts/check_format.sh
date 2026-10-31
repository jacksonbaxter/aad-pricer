#!/usr/bin/env bash
# Verifies that every tracked C++ file matches clang-format, and changes nothing.
#
#   scripts/check_format.sh
#
# clang-format comes from $CLANG_FORMAT, or from PATH. Its output changes between
# releases, so a different version is an error rather than a noisy diff.
# To apply the format: git ls-files -z '*.cpp' '*.hpp' | xargs -0 clang-format -i
set -uo pipefail

readonly PINNED_VERSION=23.1.4

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd -- "$repo_root" || exit 2

clang_format=${CLANG_FORMAT:-clang-format}
if ! command -v "$clang_format" >/dev/null 2>&1; then
  printf 'check_format: %s not found. Put LLVM %s on PATH or set CLANG_FORMAT.\n' \
    "$clang_format" "$PINNED_VERSION" >&2
  exit 2
fi
version_line=$("$clang_format" --version) || exit 2
if [[ $version_line != *"clang-format version $PINNED_VERSION"* ]]; then
  printf 'check_format: need clang-format %s, found: %s\n' "$PINNED_VERSION" "$version_line" >&2
  exit 2
fi

files=()
while IFS= read -r -d '' file; do
  files+=("$file")
done < <(git ls-files -z -- '*.cpp' '*.hpp')
if ((${#files[@]} == 0)); then
  printf 'check_format: git lists no C++ files\n' >&2
  exit 2
fi

if ! "$clang_format" --dry-run --Werror --style=file -- "${files[@]}"; then
  printf 'check_format: FAIL, see the diagnostics above\n' >&2
  exit 1
fi
printf 'check_format: %d files match clang-format %s\n' "${#files[@]}" "$PINNED_VERSION"
