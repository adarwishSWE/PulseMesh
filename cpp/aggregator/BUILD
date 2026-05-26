package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library")

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
        "//cpp/aggregator/impl:time_series_store",
        "//proto:metrics_cc",
    ],
)
