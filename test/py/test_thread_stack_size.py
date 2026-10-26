"""Checks for the unittest binary's thread-stack-size option.

The option sizes the thread stacks before any test runs, so no .test file can observe a failure to start.
Point at a binary with DUCKDB_UNITTEST_BINARY, or let discovery find
build/{debug,relassert,reldebug,release}/test/unittest under DUCKDB_ROOT (default cwd).
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

TRIVIAL = """# name: probe.test
# group: [runner]

query I
SELECT 42
----
42
"""

# Whether a probe thread overflows into a neighbouring mapping depends on the address space layout of the run.
RUNS = 10


def _discover_binary():
    explicit = os.environ.get("DUCKDB_UNITTEST_BINARY")
    if explicit:
        return Path(explicit)
    root = Path(os.environ.get("DUCKDB_ROOT", os.getcwd()))
    for flavour in ("debug", "relassert", "reldebug", "release"):
        candidate = root / "build" / flavour / "test" / "unittest"
        if candidate.is_file():
            return candidate
    return None


@pytest.fixture(scope="session")
def unittest_bin() -> Path:
    binary = _discover_binary()
    if binary is None or not binary.is_file():
        pytest.skip("no unittest binary; set DUCKDB_UNITTEST_BINARY")
    return binary


def _run(unittest_bin: Path, *args: str, wrapper: tuple = ()) -> subprocess.CompletedProcess:
    root = Path(os.environ.get("DUCKDB_ROOT", os.getcwd()))
    # An ambient DUCKDB_TEST_ option would change the configuration under test.
    ambient = {k: v for k, v in os.environ.items() if not k.startswith("DUCKDB_TEST_")}
    return subprocess.run(
        [*wrapper, str(unittest_bin), "--test-dir", str(root), "--stdin", *args],
        input=TRIVIAL,
        capture_output=True,
        text=True,
        env=ambient,
        timeout=300,
    )


@pytest.mark.parametrize("stack_size", ["131072", "8388608"])
def test_thread_stack_size_starts_the_run(unittest_bin: Path, stack_size: str):
    """Sizing the stacks probes threads on minimum size stacks, which must not crash the runner."""
    for _ in range(RUNS):
        result = _run(unittest_bin, "--thread-stack-size", stack_size)
        assert result.returncode == 0, f"exit code {result.returncode}\n{result.stdout}\n{result.stderr}"
        assert "All tests passed" in result.stdout


def test_thread_stack_size_reports_an_unusable_default_stack(unittest_bin: Path):
    """A default stack size too large to probe is reported as an error, not a crash."""
    prlimit = shutil.which("prlimit")
    if prlimit is None:
        pytest.skip("no prlimit")
    huge = str(2**64 - 32768)
    result = _run(unittest_bin, "--thread-stack-size", "131072", wrapper=(prlimit, f"--stack={huge}:{huge}", "--"))
    assert result.returncode > 0, f"exit code {result.returncode}\n{result.stdout}\n{result.stderr}"
