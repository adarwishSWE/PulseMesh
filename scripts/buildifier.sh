#!/usr/bin/env bash
# Format or check Bazel BUILD/WORKSPACE files with buildifier.
#
# Usage:
#   scripts/buildifier.sh [--check]              # all BUILD/WORKSPACE/*.bazel under repo
#   scripts/buildifier.sh [--check] path/to/BUILD ...
set -euo pipefail
cd "$(dirname "$0")/.."

resolve_buildifier() {
    if command -v buildifier >/dev/null 2>&1; then
        command -v buildifier
        return
    fi
    if [[ -x "${HOME}/.local/bin/buildifier" ]]; then
        echo "${HOME}/.local/bin/buildifier"
        return
    fi
    echo "[buildifier] buildifier not found; install to ~/.local/bin or PATH" >&2
    exit 1
}

BUILDIFIER="$(resolve_buildifier)"

MODE=format
ARGS=()
for arg in "$@"; do
    case "$arg" in
        --check)
            MODE=check
            ;;
        -*)
            echo "[buildifier] unknown option: $arg (use --check)" >&2
            exit 2
            ;;
        *)
            ARGS+=("$arg")
            ;;
    esac
done

run_buildifier() {
    if [[ "$MODE" == check ]]; then
        "$BUILDIFIER" -mode=check "$@"
    else
        "$BUILDIFIER" "$@"
    fi
}

if [[ ${#ARGS[@]} -gt 0 ]]; then
    run_buildifier "${ARGS[@]}"
    echo "[buildifier] OK (${#ARGS[@]} file(s))"
    exit 0
fi

if [[ "$MODE" == check ]]; then
    run_buildifier -r .
else
    run_buildifier -r .
fi
echo "[buildifier] OK (repository)"
