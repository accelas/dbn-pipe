"""Module extension for databento-cpp headers."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Version configuration - update these when upgrading
DATABENTO_VERSION = "0.45.0"
DATABENTO_SHA256 = "7fedb600153f1d438f8a9c38b06dff2f53af5ec312220d533e8d9b8e92601f89"

def _databento_impl(ctx):
    http_archive(
        name = "databento_cpp",
        urls = [
            "https://github.com/databento/databento-cpp/archive/refs/tags/v{}.tar.gz".format(DATABENTO_VERSION),
        ],
        strip_prefix = "databento-cpp-{}".format(DATABENTO_VERSION),
        sha256 = DATABENTO_SHA256,
        build_file = Label("//third_party/databento:BUILD.databento.bazel"),
    )

databento = module_extension(
    implementation = _databento_impl,
)
