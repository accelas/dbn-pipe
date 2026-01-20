"""Module extension for RapidJSON header-only library."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Version configuration - update these when upgrading
RAPIDJSON_VERSION = "1.1.0"
RAPIDJSON_SHA256 = "bf7ced29704a1e696fbccf2a2b4ea068e7774fa37f6d7dd4039d0787f8bed98e"

def _rapidjson_impl(ctx):
    http_archive(
        name = "rapidjson",
        urls = [
            "https://github.com/Tencent/rapidjson/archive/refs/tags/v{}.tar.gz".format(RAPIDJSON_VERSION),
        ],
        strip_prefix = "rapidjson-{}".format(RAPIDJSON_VERSION),
        sha256 = RAPIDJSON_SHA256,
        build_file = Label("//third_party/rapidjson:BUILD.rapidjson.bazel"),
    )

rapidjson_ext = module_extension(
    implementation = _rapidjson_impl,
)
