package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "time_series_store_test",
    srcs = ["time_series_store_test.cpp"],
    deps = [
        "//cpp/aggregator/impl:time_series_store",
        "@com_google_googletest//:gtest_main",
    ],
)
