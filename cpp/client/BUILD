load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "i_metric_collector",
    hdrs = ["i_metric_collector.h"],
    deps = ["//proto:metrics_cc_proto"],
)

cc_library(
    name = "agent_lib",
    deps = [
        ":i_metric_collector",
        "//cpp/client/impl/agent_config",
        "//cpp/client/impl/batch_builder",
        "//cpp/client/impl/pending_batch_queue",
        "//cpp/client/impl/proc_metric_collector",
        "//cpp/client/impl/synthetic_metric_collector",
    ],
)

cc_binary(
    name = "collector_smoke",
    srcs = ["collector_smoke.cpp"],
    deps = [
        "//cpp/client/impl/batch_builder",
        "//cpp/client/impl/pending_batch_queue",
        "//cpp/client/impl/proc_metric_collector",
    ],
)
