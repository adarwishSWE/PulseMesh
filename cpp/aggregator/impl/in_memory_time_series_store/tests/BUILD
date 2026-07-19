load("@rules_cc//cc:defs.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_test(
    name = "in_memory_time_series_store_test",
    srcs = ["in_memory_time_series_store_test.cpp"],
    deps = [
        "//cpp/aggregator/impl/in_memory_time_series_store",
        "@com_google_googletest//:gtest_main",
    ],
)
