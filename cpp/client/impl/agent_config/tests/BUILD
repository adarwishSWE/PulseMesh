load("@rules_cc//cc:defs.bzl", "cc_test")

package(default_visibility = ["//visibility:public"])

cc_test(
    name = "agent_config_test",
    srcs = ["agent_config_test.cpp"],
    deps = [
        "//cpp/client/impl/agent_config",
        "@com_google_googletest//:gtest_main",
    ],
)
