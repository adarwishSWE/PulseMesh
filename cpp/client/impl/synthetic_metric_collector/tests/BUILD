load("@rules_cc//cc:defs.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_test(
    name = "synthetic_metric_collector_test",
    srcs = ["synthetic_metric_collector_test.cpp"],
    deps = [
        "//cpp/client/impl/synthetic_metric_collector",
        "@com_google_googletest//:gtest_main",
    ],
)
