load("@rules_cc//cc:defs.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_test(
    name = "pending_batch_queue_test",
    srcs = ["pending_batch_queue_test.cpp"],
    deps = [
        "//cpp/client/impl/pending_batch_queue",
        "@com_google_googletest//:gtest_main",
    ],
)
