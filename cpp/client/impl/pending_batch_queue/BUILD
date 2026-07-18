load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "pending_batch_queue",
    srcs = ["pending_batch_queue.cpp"],
    hdrs = ["pending_batch_queue.h"],
    deps = ["//proto:metrics_cc_proto"],
)
