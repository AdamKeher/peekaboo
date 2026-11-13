#!/bin/sh
#
# Check that all project C/C++ sources are clang-format clean.
# Edits nothing; exits non-zero if any file would change (CI-friendly).
# Uses the root .clang-format; skips vendored code under src/vendor/.
#
# Usage (from anywhere):  ./scripts/format-check.sh
set -eu

# Run from the repo root regardless of the caller's cwd.
cd "$(dirname "$0")/.."

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "format-check.sh: '$CLANG_FORMAT' not found on PATH." >&2
    exit 1
fi

find src tests -type f \( -name '*.cpp' -o -name '*.h' \) ! -path 'src/vendor/*' \
    -print0 | xargs -0 "$CLANG_FORMAT" --dry-run -Werror

echo "format-check.sh: all files are clang-format clean."
