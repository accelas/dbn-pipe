"""Module extension for system OpenSSL."""

def _openssl_sys_impl(repository_ctx):
    """Creates a repository that wraps system OpenSSL."""
    repository_ctx.file("BUILD.bazel", """
cc_library(
    name = "crypto",
    hdrs = glob(["include/**/*.h"]),
    linkopts = ["-lcrypto"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "ssl",
    hdrs = glob(["include/**/*.h"]),
    linkopts = ["-lssl", "-lcrypto"],
    visibility = ["//visibility:public"],
)
""")

    # Get include path from pkg-config or use default
    result = repository_ctx.execute(["pkg-config", "--variable=includedir", "openssl"])
    if result.return_code == 0:
        include_dir = result.stdout.strip()
    else:
        include_dir = "/usr/include"

    # Symlink the include directory
    repository_ctx.symlink(include_dir + "/openssl", "include/openssl")

openssl_sys = repository_rule(
    implementation = _openssl_sys_impl,
    local = True,
)

def _openssl_sys_ext_impl(ctx):
    openssl_sys(name = "openssl_sys")

openssl_sys_ext = module_extension(
    implementation = _openssl_sys_ext_impl,
)
