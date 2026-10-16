# fmt: off

import signal
import subprocess
import sys

import pytest

resource = pytest.importorskip("resource")


def run_with_address_space_limit(shell, limit_mb, sql):
    limit = limit_mb << 20

    def limit_address_space():
        resource.setrlimit(resource.RLIMIT_AS, (limit, limit))

    return subprocess.run(
        [shell, "--batch", "--no-init", ":memory:", "-c", sql],
        capture_output=True,
        preexec_fn=limit_address_space,
        timeout=300,
    )


def check_out_of_memory(result):
    # glibc aborts by design when it cannot register the destructor of a thread_local
    if result.returncode == -signal.SIGABRT and b"failed to register TLS destructor" in result.stderr:
        return
    # otherwise the query runs or fails with an error, but the process is not killed by a signal
    assert result.returncode in (0, 1), result.stderr
    if result.returncode == 1:
        assert b"Out of Memory" in result.stderr, result.stderr


# threads that start without memory for their allocator state used to crash the process
@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="RLIMIT_AS limits the address space on Linux")
@pytest.mark.parametrize("limit_mb", [256, 512, 1024, 2048])
def test_address_space_limit(shell, limit_mb):
    for _ in range(3):
        check_out_of_memory(run_with_address_space_limit(shell, limit_mb, "SELECT 42"))


# workers out of memory while they run tasks or keep their own bookkeeping
@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="RLIMIT_AS limits the address space on Linux")
@pytest.mark.parametrize("limit_mb", [300, 500])
def test_address_space_limit_while_loading(shell, limit_mb):
    sql = (
        "CREATE TABLE x AS SELECT range AS i, range::VARCHAR || 'abcdefghij' AS s FROM range(20000000); "
        "SELECT count(*) FROM x"
    )
    for _ in range(3):
        check_out_of_memory(run_with_address_space_limit(shell, limit_mb, sql))
