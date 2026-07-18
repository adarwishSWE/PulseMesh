load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "synthetic_metric_collector",
    srcs = ["synthetic_metric_collector.cpp"],
    hdrs = ["synthetic_metric_collector.h"],
    deps = [
        "//cpp/client:i_metric_collector",
        "//proto:metrics_cc_proto",
    ],
)
