"""Module extension for DuckDB embedded database."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Version configuration - update these when upgrading
DUCKDB_VERSION = "1.4.3"
# SHA256 from bazel download
DUCKDB_SHA256 = "a9d834d07524f483aa1132ee183169767008468cc485c25b2170b9e6eee47ef6"

def _duckdb_impl(ctx):
    http_archive(
        name = "duckdb",
        urls = [
            "https://github.com/duckdb/duckdb/releases/download/v{}/libduckdb-src.zip".format(DUCKDB_VERSION),
        ],
        sha256 = DUCKDB_SHA256,
        build_file = Label("//third_party/duckdb:BUILD.duckdb.bazel"),
    )

duckdb = module_extension(
    implementation = _duckdb_impl,
)
