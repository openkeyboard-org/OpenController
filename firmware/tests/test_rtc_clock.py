"""RTC32K monotonic clock tests.

Native-compiles the pure RtcClock_Advance() from src/rtc_clock.c (the HAL
wrapper is compiled out with RTC_CLOCK_NATIVE_TEST) and drives it with raw
counter samples: wraps, stale backward reads, and both together.
"""
import ctypes
import subprocess
from pathlib import Path

import pytest

FW = Path(__file__).resolve().parent.parent
SRC = FW / "src" / "rtc_clock.c"

M = 0xA8C00000      # RTC_MAX_COUNT
BACKSTEP = 1024     # RTC_CLOCK_BACKSTEP


class Clock(ctypes.Structure):
    _fields_ = [("started", ctypes.c_uint8), ("last_raw", ctypes.c_uint32),
                ("elapsed", ctypes.c_uint32)]


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    so = tmp_path_factory.mktemp("rtcclock") / "rtc_clock.so"
    subprocess.run(
        ["cc", "-shared", "-fPIC", "-O1", "-Wall", "-Werror", "-DRTC_CLOCK_NATIVE_TEST",
         "-I", str(FW / "src"), str(SRC), "-o", str(so)],
        check=True)
    dll = ctypes.CDLL(str(so))
    dll.RtcClock_Advance.restype = ctypes.c_uint32
    dll.RtcClock_Advance.argtypes = [ctypes.POINTER(Clock), ctypes.c_uint32,
                                     ctypes.c_uint32, ctypes.c_uint32]
    return dll


def run(lib, samples):
    clk = Clock()
    return [lib.RtcClock_Advance(ctypes.byref(clk), s, M, BACKSTEP) for s in samples]


def test_first_sample_is_time_zero(lib):
    assert run(lib, [123456]) == [0]


def test_forward_steps_accumulate(lib):
    assert run(lib, [100, 110, 150, 150]) == [0, 10, 50, 50]


def test_a_small_backward_read_adds_nothing(lib):
    # 200 -> 197 is a stale read; time resumes from the newest value, 200.
    assert run(lib, [100, 200, 197, 201]) == [0, 100, 100, 101]


def test_the_wrap_is_a_small_forward_step(lib):
    assert run(lib, [M - 3, 2]) == [0, 5]


def test_a_stale_read_across_the_wrap_is_not_a_huge_advance(lib):
    """Codex's sequence: M-2 -> 1 -> M-1 -> 2 is four ticks of real time."""
    assert run(lib, [M - 2, 1, M - 1, 2]) == [0, 3, 3, 4]


def test_a_backstep_just_inside_the_tolerance_is_ignored(lib):
    assert run(lib, [5000, 5000 - BACKSTEP]) == [0, 0]


def test_elapsed_time_runs_past_one_counter_period(lib):
    step = M // 4
    samples = [0, step, 2 * step, 3 * step, (4 * step) % M, step]
    out = run(lib, samples)
    assert out[-1] == (5 * step) & 0xFFFFFFFF
