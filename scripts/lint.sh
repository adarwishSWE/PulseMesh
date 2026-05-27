#!/usr/bin/env bash
# Run buildifier, clang-format, and clang-tidy.
set -euo pipefail
ROOT="$(dirname "$0")"
COMMAND=all
BUILDIFIER_ARGS=()
FORMAT_ARGS=()
TIDY_ARGS=()

usage() {
    cat <<'EOF'
Usage: scripts/lint.sh [command] [options]

Commands:
  all          buildifier, then clang-format, then clang-tidy (default)
  bazel        buildifier only
  format       clang-format only
  tidy         clang-tidy only
  cpp          clang-format then clang-tidy

Options:
  --check      Verify only (buildifier / clang-format; no file writes)
  --fix        Apply clang-tidy fixes where available (tidy / all / cpp)

Examples:
  scripts/lint.sh
  scripts/lint.sh bazel --check
  scripts/lint.sh cpp --check

Git hook (once per clone):
  scripts/install_git_hooks.sh
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check)
            BUILDIFIER_ARGS=(--check)
            FORMAT_ARGS=(--check)
            ;;
        --fix)
            TIDY_ARGS=(--fix)
            ;;
        all | bazel | format | tidy | cpp)
            COMMAND="$1"
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "[lint] unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

case "$COMMAND" in
    bazel)
        "$ROOT/buildifier.sh" "${BUILDIFIER_ARGS[@]}"
        ;;
    format)
        "$ROOT/clang_format.sh" "${FORMAT_ARGS[@]}"
        ;;
    tidy)
        "$ROOT/clang_tidy.sh" "${TIDY_ARGS[@]}"
        ;;
    cpp)
        "$ROOT/clang_format.sh" "${FORMAT_ARGS[@]}"
        "$ROOT/clang_tidy.sh" "${TIDY_ARGS[@]}"
        ;;
    all)
        "$ROOT/buildifier.sh" "${BUILDIFIER_ARGS[@]}"
        "$ROOT/clang_format.sh" "${FORMAT_ARGS[@]}"
        "$ROOT/clang_tidy.sh" "${TIDY_ARGS[@]}"
        ;;
    *)
        echo "[lint] usage: $0 [all|bazel|format|tidy|cpp] [--check] [--fix]" >&2
        exit 2
        ;;
esac
