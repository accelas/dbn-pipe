"""Module extension for ASIO header-only library."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Version configuration - update these when upgrading
# Using asio-1-34-2 which corresponds to standalone ASIO 1.34.2
ASIO_VERSION = "1-34-2"
ASIO_SHA256 = "f3bac015305fbb700545bd2959fbc52d75a1ec2e05f9c7f695801273ceb78cf5"

def _asio_impl(ctx):
    http_archive(
        name = "asio",
        urls = [
            "https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-{}.tar.gz".format(ASIO_VERSION),
        ],
        strip_prefix = "asio-asio-{}/asio".format(ASIO_VERSION),
        sha256 = ASIO_SHA256,
        build_file = Label("//third_party/asio:BUILD.asio.bazel"),
    )

asio_ext = module_extension(
    implementation = _asio_impl,
)
