#!/bin/sh
#
# Format all project C/C++ sources in place with clang-format.
# Uses the root .clang-format; skips vendored code under src/vendor/.
#
# Usage (from anywhere):  ./scripts/format.sh
set -eu

# Run from the repo root regardless of the caller's cwd.
cd "$(dirname "$0")/.."

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "format.sh: '$CLANG_FORMAT' not found on PATH." >&2
    exit 1
fi

find src tests -type f \( -name '*.cpp' -o -name '*.h' \) ! -path 'src/vendor/*' \
    -print0 | xargs -0 "$CLANG_FORMAT" -i

echo "format.sh: done."
