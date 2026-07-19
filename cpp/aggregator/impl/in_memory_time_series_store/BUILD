load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "in_memory_time_series_store",
    srcs = ["in_memory_time_series_store.cpp"],
    hdrs = ["in_memory_time_series_store.h"],
    deps = [
        "//cpp/aggregator:i_time_series_store",
        "//proto:metrics_cc_proto",
    ],
)
