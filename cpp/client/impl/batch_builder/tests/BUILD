load("@rules_cc//cc:defs.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_test(
    name = "batch_builder_test",
    srcs = ["batch_builder_test.cpp"],
    deps = [
        "//cpp/client/impl/batch_builder",
        "@com_google_googletest//:gtest_main",
    ],
)
