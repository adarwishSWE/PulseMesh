#!/usr/bin/env bash
# Regenerate Python protobuf + gRPC stubs from proto/metrics.proto.
# Uses the Protobuf and gRPC generators pinned by the Bazel workspace.
# Run after any .proto change (requires explicit approval per freeze policy).
set -euo pipefail
cd "$(dirname "$0")/.."

PROTO_DIR="proto"
OUT_DIR="python/generated"

if command -v bazelisk >/dev/null 2>&1; then
    BAZEL=bazelisk
elif [[ -x "${BAZELISK:-}" ]]; then
    BAZEL="$BAZELISK"
else
    echo "[regenerate_python_stubs] bazelisk not found; install it or set BAZELISK" >&2
    exit 1
fi

"$BAZEL" build \
    @com_google_protobuf//:protoc \
    @com_github_grpc_grpc//src/compiler:grpc_python_plugin

BAZEL_BIN="$("$BAZEL" info bazel-bin)"
PROTOC="$BAZEL_BIN/external/com_google_protobuf/protoc"
GRPC_PYTHON_PLUGIN="$BAZEL_BIN/external/com_github_grpc_grpc/src/compiler/grpc_python_plugin"

mkdir -p "$OUT_DIR"

"$PROTOC" \
    --proto_path="$PROTO_DIR" \
    --plugin=protoc-gen-grpc_python="$GRPC_PYTHON_PLUGIN" \
    --python_out="$OUT_DIR" \
    --grpc_python_out="$OUT_DIR" \
    "$PROTO_DIR/metrics.proto"

# The generator emits a bare import, which fails inside the python.generated package.
sed -i 's/import metrics_pb2 as metrics__pb2/from python.generated import metrics_pb2 as metrics__pb2/' \
    "$OUT_DIR/metrics_pb2_grpc.py"

# Ensure python/generated/ is a valid package
touch "$OUT_DIR/__init__.py"

echo "[regenerate_python_stubs] generated stubs in $OUT_DIR/"
