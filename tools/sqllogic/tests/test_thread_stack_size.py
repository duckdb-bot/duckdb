import shutil
import subprocess
import sys

import pytest

TRIVIAL = """query I
SELECT 42
----
42
"""

# Whether a probe thread overflows into a neighbouring mapping depends on the address space layout of the run.
RUNS = 10


def run_with_stack_size(unittest_binary, stack_size, wrapper=()):
    result = subprocess.run(
        [*wrapper, unittest_binary, "--stdin", "--thread-stack-size", stack_size],
        input=TRIVIAL,
        capture_output=True,
        text=True,
        timeout=300,
    )
    if "--thread-stack-size is only supported" in result.stderr:
        pytest.skip("the runner does not support --thread-stack-size on this platform")
    return result


@pytest.mark.parametrize("stack_size", ["131072", "8388608"])
def test_thread_stack_size_starts_the_run(unittest_binary, stack_size):
    """Sizing the stacks probes threads on minimum size stacks, which must not crash the runner."""
    for _ in range(RUNS):
        result = run_with_stack_size(unittest_binary, stack_size)
        assert result.returncode == 0, f"exit code {result.returncode}\n{result.stdout}\n{result.stderr}"
        assert "All tests passed" in result.stdout


def test_thread_stack_size_reports_an_unusable_default_stack(unittest_binary):
    """A default stack size too large to probe is reported as an error, not a crash."""
    prlimit = shutil.which("prlimit")
    if prlimit is None or sys.maxsize <= 2**32:
        pytest.skip("needs prlimit and 64 bit sizes")
    huge = str(2**64 - 32768)
    result = run_with_stack_size(unittest_binary, "131072", wrapper=(prlimit, f"--stack={huge}:{huge}", "--"))
    if "ASan cannot proceed" in result.stderr:
        pytest.skip("AddressSanitizer cannot start with this stack limit")
    assert result.returncode == 1, f"exit code {result.returncode}\n{result.stdout}\n{result.stderr}"
    assert f"Failed to map a {huge} byte probe stack" in result.stderr
