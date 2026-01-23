"""Module extension for system libpq (PostgreSQL client library)."""

def _libpq_impl(repository_ctx):
    """Creates a repository that wraps system libpq."""
    repository_ctx.file("BUILD.bazel", """
cc_library(
    name = "libpq",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    linkopts = ["-lpq"],
    visibility = ["//visibility:public"],
)
""")

    # Get include path from pg_config or use default
    result = repository_ctx.execute(["pg_config", "--includedir"])
    if result.return_code == 0:
        include_dir = result.stdout.strip()
    else:
        include_dir = "/usr/include/postgresql"

    # Symlink the libpq header
    repository_ctx.symlink(include_dir + "/libpq-fe.h", "include/libpq-fe.h")
    # Also include the postgres_ext.h which libpq-fe.h depends on
    repository_ctx.symlink(include_dir + "/postgres_ext.h", "include/postgres_ext.h")

libpq_sys = repository_rule(
    implementation = _libpq_impl,
    local = True,
)

def _libpq_ext_impl(ctx):
    libpq_sys(name = "libpq")

libpq_ext = module_extension(
    implementation = _libpq_ext_impl,
)
