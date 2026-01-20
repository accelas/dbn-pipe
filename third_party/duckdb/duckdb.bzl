"""Module extension for DuckDB embedded database."""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Version configuration - update these when upgrading
DUCKDB_VERSION = "1.4.3"
# SHA256 for pre-built Linux amd64 binary distribution
DUCKDB_SHA256 = "3c7bb8d586d39ccce56442c3f6bafa97f53d7b6bb5405dac827442fcb31494aa"

def _duckdb_impl(ctx):
    http_archive(
        name = "duckdb",
        urls = [
            # Use pre-built binary to avoid memory-intensive compilation
            "https://github.com/duckdb/duckdb/releases/download/v{}/libduckdb-linux-amd64.zip".format(DUCKDB_VERSION),
        ],
        sha256 = DUCKDB_SHA256,
        build_file = Label("//third_party/duckdb:BUILD.duckdb.bazel"),
    )

duckdb = module_extension(
    implementation = _duckdb_impl,
)
