load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//cpp/tests:__subpackages__"])

cc_library(
    name = "mock_time_series_store",
    hdrs = ["mock_time_series_store.h"],
    deps = [
        "//cpp/aggregator:i_time_series_store",
        "@com_google_googletest//:gtest",
    ],
)
