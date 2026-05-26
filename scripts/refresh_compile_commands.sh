#!/usr/bin/env bash
# Regenerate compile_commands.json for clangd / IDE indexing.
# Requires Bazelisk (honours .bazelversion) — plain `bazel` may be the wrong version.
set -euo pipefail
cd "$(dirname "$0")/.."

if command -v bazelisk >/dev/null 2>&1; then
  BAZEL=bazelisk
elif [[ -x "${BAZELISK:-}" ]]; then
  BAZEL="$BAZELISK"
else
  echo "[refresh_compile_commands] bazelisk not found; install it or set BAZELISK" >&2
  exit 1
fi

"$BAZEL" build //cpp/... //proto/...
"$BAZEL" run @hedron_compile_commands//:refresh_all
echo "[refresh_compile_commands] wrote $(pwd)/compile_commands.json"
