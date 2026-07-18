load("@rules_cc//cc:defs.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_test(
    name = "proc_metric_collector_test",
    srcs = ["proc_metric_collector_test.cpp"],
    deps = [
        "//cpp/client/impl/proc_metric_collector",
        "@com_google_googletest//:gtest_main",
    ],
)
