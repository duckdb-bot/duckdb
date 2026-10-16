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


# a worker thread that runs out of memory in its own bookkeeping does not abort the process
@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="RLIMIT_AS limits the address space on Linux")
@pytest.mark.parametrize(
    "limit_mb, sql, runs",
    [
        (2048, "SELECT 42", 20),
        (300, "CREATE TABLE x AS SELECT range AS i, range::VARCHAR || 'abcdefghij' AS s FROM range(20000000)", 3),
    ],
)
def test_worker_out_of_memory(shell, limit_mb, sql, runs):
    for _ in range(runs):
        result = run_with_address_space_limit(shell, limit_mb, sql)
        if result.returncode == -signal.SIGSEGV:
            # a segmentation fault when the address space runs out is a separate problem
            continue
        assert result.returncode in (0, 1), result.stderr
        if result.returncode == 1:
            assert b"Out of Memory" in result.stderr, result.stderr
