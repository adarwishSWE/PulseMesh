#!/usr/bin/env bash
# Install tracked Git hooks into .git/hooks/ (run once per clone).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOOK_SRC="$ROOT/scripts/hooks/pre-commit"
HOOK_DST="$ROOT/.git/hooks/pre-commit"

if [[ ! -d "$ROOT/.git" ]]; then
    echo "[install_git_hooks] not a git repository: $ROOT" >&2
    exit 1
fi

if [[ ! -f "$HOOK_SRC" ]]; then
    echo "[install_git_hooks] missing $HOOK_SRC" >&2
    exit 1
fi

chmod +x "$HOOK_SRC"
ln -sf "$HOOK_SRC" "$HOOK_DST"
chmod +x "$HOOK_DST"

echo "[install_git_hooks] installed pre-commit -> scripts/hooks/pre-commit"
echo "[install_git_hooks] each commit: buildifier on staged BUILD/WORKSPACE/*.bzl,"
echo "[install_git_hooks]              clang-format on staged cpp/**/*.{cpp,h},"
echo "[install_git_hooks]              clang-tidy on staged cpp/**/*.cpp"
echo "[install_git_hooks] requires buildifier (~/.local/bin) and compile_commands.json"
