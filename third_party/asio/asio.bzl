"""Module extension for system ASIO header-only library."""

def _asio_impl(repository_ctx):
    """Creates a repository that wraps system ASIO headers."""
    repository_ctx.file("BUILD.bazel", """
cc_library(
    name = "asio",
    hdrs = glob(["include/**/*.hpp", "include/**/*.ipp"]),
    includes = ["include"],
    defines = [
        "ASIO_STANDALONE",
        "ASIO_HAS_CO_AWAIT",
    ],
    visibility = ["//visibility:public"],
)
""")

    # Get include path from pkg-config or use default
    result = repository_ctx.execute(["pkg-config", "--variable=includedir", "asio"])
    if result.return_code == 0:
        include_dir = result.stdout.strip()
    else:
        include_dir = "/usr/include"

    # Symlink the ASIO include directory
    repository_ctx.symlink(include_dir + "/asio", "include/asio")
    repository_ctx.symlink(include_dir + "/asio.hpp", "include/asio.hpp")

asio_sys = repository_rule(
    implementation = _asio_impl,
    local = True,
)

def _asio_ext_impl(ctx):
    asio_sys(name = "asio")

asio_ext = module_extension(
    implementation = _asio_ext_impl,
)
