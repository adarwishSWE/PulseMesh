package(default_visibility = ["//visibility:public"])

load("@com_github_grpc_grpc//bazel:cc_grpc_library.bzl", "cc_grpc_library")

# ------------------------------------------------------------------------------
# C++ proto + gRPC stubs
# ------------------------------------------------------------------------------

proto_library(
    name = "metrics_proto",
    srcs = ["metrics.proto"],
)

cc_proto_library(
    name = "metrics_cc_proto",
    deps = [":metrics_proto"],
)

cc_grpc_library(
    name = "metrics_cc_grpc",
    srcs = [":metrics_proto"],
    grpc_only = True,
    deps = [":metrics_cc_proto"],
)

# Convenience bundle
cc_library(
    name = "metrics_cc",
    deps = [
        ":metrics_cc_proto",
        ":metrics_cc_grpc",
    ],
)

# Python stubs are generated outside Bazel via grpcio-tools (see scripts/regenerate_python_stubs.sh).
# They live in python/generated/ and are used directly by the CLI and dashboard.
