#!/usr/bin/env bash
# Run clang-tidy using repo-root .clang-tidy and compile_commands.json.
#
# Usage:
#   scripts/clang_tidy.sh [--fix]              # all cpp/**/*.cpp
#   scripts/clang_tidy.sh [--fix] path/to/a.cpp ...
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ ! -f compile_commands.json ]]; then
    echo "[clang_tidy] compile_commands.json missing; run scripts/refresh_compile_commands.sh" >&2
    exit 1
fi

RUN_CLANG_TIDY=${RUN_CLANG_TIDY:-run-clang-tidy-19}
if ! command -v "$RUN_CLANG_TIDY" >/dev/null 2>&1; then
    echo "[clang_tidy] $RUN_CLANG_TIDY not found; clang-tidy 19+ is required for C++23 std::expected" >&2
    exit 1
fi

EXTRA_ARGS=()
ARGS=()
for arg in "$@"; do
    case "$arg" in
        --fix)
            EXTRA_ARGS=(-fix)
            ;;
        -*)
            echo "[clang_tidy] unknown option: $arg (use --fix)" >&2
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
    mapfile -t FILES < <(find cpp -type f -name '*.cpp' | sort)
fi
if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "[clang_tidy] no .cpp sources under cpp/" >&2
    exit 0
fi

# Hedron's extracted command omits Protobuf's transitive source include root and
# may retain a dependency's earlier language flag. Supply both explicitly so
# clang-tidy uses the project contract: Bazel-pinned headers parsed as C++23.
PROTOBUF_INCLUDE='-isystemexternal/com_google_protobuf/src'
PROJECT_STANDARD='-std=c++23'

# HeaderFilterRegex in .clang-tidy restricts diagnostics to cpp/; pass explicitly for run-clang-tidy.
"$RUN_CLANG_TIDY" -p . -header-filter='^cpp/' -warnings-as-errors='*' -quiet \
    -extra-arg="${PROTOBUF_INCLUDE}" -extra-arg="${PROJECT_STANDARD}" "${EXTRA_ARGS[@]}" \
    "${FILES[@]}"
echo "[clang_tidy] OK (${#FILES[@]} files)"
