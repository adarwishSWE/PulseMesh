load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "i_time_series_store",
    hdrs = ["i_time_series_store.h"],
    deps = [
        "//proto:metrics_cc_proto",
    ],
)

cc_library(
    name = "aggregator_lib",
    deps = [
        "//cpp/aggregator/impl/in_memory_time_series_store",
        "//proto:metrics_cc",
    ],
)
