licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

envoy_package()

envoy_cc_library(
    name = "well_known_names",
    hdrs = ["well_known_names.h"],
    deps = [
        "//source/common/singleton:const_singleton",
    ],
)

envoy_cc_library(
    name = "token_file",
    srcs = ["token_file.cc"],
    hdrs = ["token_file.h"],
    external_deps = ["grpc"],
    deps = [
        "//include/envoy/grpc:google_grpc_creds_interface",
        "//include/envoy/registry",
        "//source/common/common:assert_lib",
        "//source/common/grpc:google_grpc_creds_lib",
        "//source/extensions/grpc_credentials:well_known_names",
    ],
)
