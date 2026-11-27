import os
import shutil
import subprocess
import sys

import pytest

from conftest import ShellTest

# Whether a probe thread overflows into a neighbouring mapping depends on the address space layout of the run.
RUNS = 10

# Sizes of glibc's optional static TLS surplus (512 by default), part of the static TLS atop a created thread's stack.
STATIC_TLS_SURPLUS_SIZES = range(512, 4608, 512)
RUNS_PER_STATIC_TLS_SURPLUS = 3


def run_with_stack_size(shell, stack_size, wrapper=(), environment=None):
    env = dict(os.environ)
    # agent-mode variables of the runner must not switch the shell under test into agent mode
    for key in ShellTest.AGENT_ENVIRONMENT_VARIABLES:
        env.pop(key, None)
    env.update(environment or {})
    result = subprocess.run(
        [*wrapper, shell, "--batch", "--no-init", "--thread-stack-size", stack_size, "-csv", "-c", "SELECT 42"],
        capture_output=True,
        text=True,
        env=env,
        timeout=300,
    )
    if "--thread-stack-size is only supported" in result.stderr:
        pytest.skip("the shell does not support --thread-stack-size on this platform")
    return result


def check_shell_started(result):
    assert result.returncode == 0, f"exit code {result.returncode}\n{result.stdout}\n{result.stderr}"
    assert "42" in result.stdout


@pytest.mark.parametrize("stack_size", ["131072", "8388608"])
def test_thread_stack_size_starts_the_shell(shell, stack_size):
    """Sizing the stacks probes threads on minimum size stacks, which must not crash the shell."""
    for _ in range(RUNS):
        check_shell_started(run_with_stack_size(shell, stack_size))


@pytest.mark.parametrize("static_tls_surplus", STATIC_TLS_SURPLUS_SIZES)
def test_thread_stack_size_starts_the_shell_across_static_tls_sizes(shell, static_tls_surplus):
    """The room a minimum size stack leaves depends on the static TLS size, sampled here every 512 bytes over 4 KiB."""
    tunables = {"GLIBC_TUNABLES": f"glibc.rtld.optional_static_tls={static_tls_surplus}"}
    for _ in range(RUNS_PER_STATIC_TLS_SURPLUS):
        check_shell_started(run_with_stack_size(shell, "131072", environment=tunables))


def test_thread_stack_size_reports_an_unusable_default_stack(shell):
    """A default stack size too large to probe is reported as an error, not a crash."""
    prlimit = shutil.which("prlimit")
    if prlimit is None or sys.maxsize <= 2**32:
        pytest.skip("needs prlimit and 64 bit sizes")
    # glibc rounds the limit up to the page size, so the value stays aligned on 4, 16 and 64 KiB pages
    huge = str(2**64 - 65536)
    result = run_with_stack_size(shell, "131072", wrapper=(prlimit, f"--stack={huge}:{huge}", "--"))
    if "ASan cannot proceed" in result.stderr:
        pytest.skip("AddressSanitizer cannot start with this stack limit")
    assert result.returncode == 1, f"exit code {result.returncode}\n{result.stdout}\n{result.stderr}"
    assert f"Failed to map a {huge} byte probe stack" in result.stderr
