#!/bin/bash
# Regenerate Python protobuf + gRPC stubs from proto/metrics.proto.
# Requires: grpcio-tools (run inside venv: source venv/bin/activate first).
# Run after any .proto change (requires explicit approval per freeze policy).
set -e

PROTO_DIR="proto"
OUT_DIR="python/generated"
PYTHON="${PYTHON:-python}"

if ! $PYTHON -m grpc_tools.protoc --version &>/dev/null; then
    echo "[error] grpcio-tools not found. Activate venv and run: pip install grpcio-tools"
    exit 1
fi

mkdir -p "$OUT_DIR"

$PYTHON -m grpc_tools.protoc \
    --proto_path="$PROTO_DIR" \
    --python_out="$OUT_DIR" \
    --grpc_python_out="$OUT_DIR" \
    "$PROTO_DIR/metrics.proto"

# Fix grpcio-tools generated import: it generates a bare `import metrics_pb2`
# which fails when the file lives inside python/generated/.
sed -i 's/import metrics_pb2 as metrics__pb2/from python.generated import metrics_pb2 as metrics__pb2/' \
    "$OUT_DIR/metrics_pb2_grpc.py"

# Ensure python/generated/ is a valid package
touch "$OUT_DIR/__init__.py"

echo "Python stubs regenerated in $OUT_DIR/"
