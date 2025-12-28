"""Module extension for llhttp HTTP parser."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Version configuration - update these when upgrading
LLHTTP_VERSION = "9.3.0"
LLHTTP_SHA256 = "1a2b45cb8dda7082b307d336607023aa65549d6f060da1d246b1313da22b685a"

def _llhttp_impl(ctx):
    http_archive(
        name = "llhttp",
        urls = [
            "https://github.com/nodejs/llhttp/archive/refs/tags/release/v{}.tar.gz".format(LLHTTP_VERSION),
        ],
        strip_prefix = "llhttp-release-v{}".format(LLHTTP_VERSION),
        sha256 = LLHTTP_SHA256,
        build_file = Label("//third_party/llhttp:BUILD.llhttp.bazel"),
    )

llhttp_ext = module_extension(
    implementation = _llhttp_impl,
)
