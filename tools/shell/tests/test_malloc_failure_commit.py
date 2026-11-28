# Fail one allocation of the main thread shortly after the write ahead log of a commit is synced: the commit either
# completes and reports success, or fails and leaves nothing behind. It must never report a failure for a table that is
# visible and durable, and the transaction must not stay active, which would block later checkpoints.

import os
import shutil
import subprocess
import sys

import pytest

from conftest import ShellTest

pytestmark = pytest.mark.skipif(
    not sys.platform.startswith("linux"), reason="the malloc shim is preloaded with LD_PRELOAD"
)

# LD_PRELOAD shim: with FAILMALLOC_AFTER_FSYNC=M:k, the k-th allocation the main thread makes after its M-th fsync or
# fdatasync fails once with ENOMEM; every other request is served by glibc. With FAILMALLOC_FSYNC_REPORT set, the main
# thread's fsync count is printed at exit.
SHIM_SOURCE = r"""
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);

static size_t target_sync, target_alloc, syncs, allocs;
static int armed, done;

static int main_thread(void) {
	return (pid_t)syscall(SYS_gettid) == getpid();
}

static int fail_now(void) {
	if (!armed || done || !main_thread()) {
		return 0;
	}
	if (++allocs != target_alloc) {
		return 0;
	}
	done = 1;
	errno = ENOMEM;
	return 1;
}

void *malloc(size_t n) {
	return fail_now() ? NULL : __libc_malloc(n);
}

void *calloc(size_t a, size_t b) {
	return fail_now() ? NULL : __libc_calloc(a, b);
}

void *realloc(void *p, size_t n) {
	return fail_now() ? NULL : __libc_realloc(p, n);
}

static void sync_seen(void) {
	if (!target_sync) {
		const char *spec = getenv("FAILMALLOC_AFTER_FSYNC");
		const char *colon = spec ? strchr(spec, ':') : NULL;
		if (spec && colon) {
			target_sync = strtoull(spec, NULL, 10);
			target_alloc = strtoull(colon + 1, NULL, 10);
		}
	}
	if (!main_thread()) {
		return;
	}
	if (++syncs == target_sync && target_alloc) {
		armed = 1;
	}
}

int fsync(int fd) {
	sync_seen();
	return ((int (*)(int))dlsym(RTLD_NEXT, "fsync"))(fd);
}

int fdatasync(int fd) {
	sync_seen();
	return ((int (*)(int))dlsym(RTLD_NEXT, "fdatasync"))(fd);
}

__attribute__((destructor)) static void report(void) {
	if (getenv("FAILMALLOC_FSYNC_REPORT")) {
		char buf[64];
		int n = snprintf(buf, sizeof(buf), "failmalloc_main_thread_fsyncs=%zu\n", syncs);
		write(2, buf, (size_t)n);
	}
}
"""

# one thread keeps the allocation sequence of a commit repeatable, and the database does not checkpoint at exit, so the
# last write ahead log sync of a script that stops after its writes belongs to its last commit. The table name is
# qualified with the database, so that the lookup does not involve the temp catalog, whose own transaction commits
# after the database's one
DATABASE_NAME = "failmalloc.db"
PREFIX_SQL = "SET threads = 1;\nPRAGMA disable_checkpoint_on_shutdown;\n"
TABLE = "failmalloc.main.t"
# a table created by one commit, and a table filled by many small commits, each of which queues its cleanup
SCENARIOS = {
    "create": ("CREATE TABLE %s AS SELECT range AS i, repeat('x', 100) AS s FROM range(200000);\n" % TABLE, b"200000"),
    "inserts": (
        "CREATE TABLE %s (i INTEGER);\n" % TABLE
        + "".join("INSERT INTO %s VALUES (%d);\n" % (TABLE, i) for i in range(20)),
        b"20",
    ),
}
# the count shows whether the table is visible, and the checkpoint must not be blocked by a transaction that stayed active
PROBE_SQL = "SELECT count(*) AS rows_in_session FROM %s;\nCHECKPOINT;\n" % TABLE
REOPEN_SQL = "SELECT count(*) AS rows_after_reopen FROM %s;" % TABLE


def run_shell(shell, sql, database, shim=None, after_fsync=None, report=False):
    env = dict(os.environ)
    # agent-mode variables of the runner must not switch the shell under test into agent mode
    for key in ShellTest.AGENT_ENVIRONMENT_VARIABLES:
        env.pop(key, None)
    env.pop("LD_PRELOAD", None)
    env.pop("FAILMALLOC_AFTER_FSYNC", None)
    env.pop("FAILMALLOC_FSYNC_REPORT", None)
    if shim is not None:
        env["LD_PRELOAD"] = shim
    if after_fsync is not None:
        env["FAILMALLOC_AFTER_FSYNC"] = after_fsync
    if report:
        env["FAILMALLOC_FSYNC_REPORT"] = "1"
    return subprocess.run(
        [shell, "--batch", "--no-init", "--csv", database],
        input=sql.encode(),
        capture_output=True,
        env=env,
        timeout=300,
    )


@pytest.fixture(scope="module")
def fsync_shim(tmp_path_factory):
    compiler = os.environ.get("CC")
    if not compiler or not shutil.which(compiler):
        compiler = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
    if not compiler:
        pytest.skip("no C compiler to build the malloc shim")
    directory = tmp_path_factory.mktemp("failmalloc_fsync")
    source = directory / "failmalloc_fsync.c"
    source.write_text(SHIM_SOURCE)
    shim = directory / "failmalloc_fsync.so"
    build = subprocess.run(
        [compiler, "-shared", "-fPIC", "-O2", "-o", str(shim), str(source), "-ldl"],
        capture_output=True,
        text=True,
    )
    if build.returncode != 0:
        pytest.skip("cannot build the malloc shim: " + build.stderr)
    return str(shim)


# the main thread's fsync that syncs the write ahead log of a scenario's last commit: the last fsync of a run of the
# scenario's writes alone, since the database does not checkpoint at exit
_commit_fsyncs = {}


def commit_fsync(request, shim, tmp_path_factory, scenario):
    if scenario in _commit_fsyncs:
        return _commit_fsyncs[scenario]
    shell = request.config.getoption("--shell-binary")
    if not shell:
        raise ValueError("Please provide a shell binary path to the tester, using '--shell-binary <path_to_cli>'")
    database = str(tmp_path_factory.mktemp("failmalloc_commit") / DATABASE_NAME)
    result = run_shell(shell, PREFIX_SQL + SCENARIOS[scenario][0], database, shim=shim, report=True)
    assert result.returncode == 0, result.stderr
    prefix = b"failmalloc_main_thread_fsyncs="
    line = next((l for l in result.stderr.splitlines() if l.startswith(prefix)), None)
    if line is None or int(line[len(prefix) :]) == 0:
        pytest.skip("the malloc shim does not intercept the file syncs of this shell binary")
    _commit_fsyncs[scenario] = int(line[len(prefix) :])
    return _commit_fsyncs[scenario]


# the k-th allocation after the sync fails: in the bookkeeping that follows the commit, or in the statements after it.
# A failure in the statement after its commit makes the shell exit before the table is selected, which is fine
@pytest.mark.parametrize("scenario", sorted(SCENARIOS))
@pytest.mark.parametrize("allocation", range(1, 41))
def test_malloc_failure_after_commit_sync(request, shell, fsync_shim, tmp_path, tmp_path_factory, scenario, allocation):
    fsync = commit_fsync(request, fsync_shim, tmp_path_factory, scenario)
    sql, rows = SCENARIOS[scenario]
    database = str(tmp_path / DATABASE_NAME)
    result = run_shell(
        shell, PREFIX_SQL + sql + PROBE_SQL, database, shim=fsync_shim, after_fsync="%d:%d" % (fsync, allocation)
    )
    assert result.returncode in (0, 1), (result.returncode, result.stderr)
    assert b"terminate called" not in result.stderr, result.stderr
    visible = rows in result.stdout
    # a commit that reports a failure has not happened
    assert not (b"Failed to commit" in result.stderr and visible), (result.stdout, result.stderr)
    # the transaction left the active set, so the checkpoint is not blocked by it
    assert b"other write transactions active" not in result.stderr, result.stderr
    # a table that is visible in the session is durable
    if visible:
        reopened = run_shell(shell, REOPEN_SQL, database)
        assert rows in reopened.stdout, (result.stdout, result.stderr, reopened.stdout, reopened.stderr)
