#!/usr/bin/env bash
# Apply or check .clang-format on C++ sources under cpp/.
# Uses the repo-root .clang-format (--style=file).
#
# Usage:
#   scripts/clang_format.sh [--check]              # all cpp/**/*.cpp, cpp/**/*.h
#   scripts/clang_format.sh [--check] path/to/a.cpp ...
set -euo pipefail
cd "$(dirname "$0")/.."

MODE=apply
ARGS=()
for arg in "$@"; do
    case "$arg" in
        --check)
            MODE=check
            ;;
        -*)
            echo "[clang_format] unknown option: $arg (use --check)" >&2
            exit 2
            ;;
        *)
            ARGS+=("$arg")
            ;;
    esac
done

if [[ ${#ARGS[@]} -gt 0 ]]; then
    FILES=("${ARGS[@]}")
else
    mapfile -t FILES < <(find cpp -type f \( -name '*.cpp' -o -name '*.h' \) | sort)
fi

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "[clang_format] no files to format" >&2
    exit 0
fi

if [[ "$MODE" == check ]]; then
    clang-format --style=file --dry-run --Werror "${FILES[@]}"
    echo "[clang_format] OK (${#FILES[@]} files)"
else
    clang-format --style=file -i "${FILES[@]}"
    echo "[clang_format] formatted ${#FILES[@]} files"
fi
