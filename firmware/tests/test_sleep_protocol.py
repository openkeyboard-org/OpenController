"""MR6 UART sleep-protocol reducer tests.

Native-compiles the HAL-free reducer (src/sleep_protocol.c) with the host
compiler and drives it through ctypes, so the capability gate and the
arbitration state machine are exercised without hardware.
"""
import ctypes
import subprocess
from pathlib import Path

import pytest

FW = Path(__file__).resolve().parent.parent
SRC = FW / "src" / "sleep_protocol.c"

# Mirror the enum / opcodes from sleep_protocol.h.
NONE, SEND_READY, ARM, CANCEL = 0, 1, 2, 3
A6, A1, A9, ACK = 0xA6, 0xA1, 0xA9, 0x61
SUB_UNLOCK, SUB_SLEEP, SUB_AUTO = 0x56, 0x54, 0x57


class State(ctypes.Structure):
    _fields_ = [("unlocked", ctypes.c_uint8), ("sleep_pending", ctypes.c_uint8),
                ("autosleep", ctypes.c_uint8)]


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    so = tmp_path_factory.mktemp("sleepproto") / "sleep_protocol.so"
    subprocess.run(
        ["cc", "-shared", "-fPIC", "-O1", "-I", str(FW / "src"),
         str(SRC), "-o", str(so)],
        check=True)
    dll = ctypes.CDLL(str(so))
    dll.SleepProtocol_OnFrame.restype = ctypes.c_int
    dll.SleepProtocol_OnFrame.argtypes = [
        ctypes.POINTER(State), ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]
    dll.SleepProtocol_Reset.argtypes = [ctypes.POINTER(State)]
    dll.SleepProtocol_IsStateChanging.restype = ctypes.c_uint8
    dll.SleepProtocol_IsStateChanging.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
    dll.SleepProtocol_ElapsedTicks.restype = ctypes.c_uint32
    dll.SleepProtocol_ElapsedTicks.argtypes = [ctypes.c_uint32] * 4
    return dll


@pytest.fixture
def st(lib):
    s = State()
    lib.SleepProtocol_Reset(ctypes.byref(s))
    return s


def feed(lib, st, cmd, sub=0, ob=0):
    return lib.SleepProtocol_OnFrame(ctypes.byref(st), cmd, sub, ob)


def test_reset_clears(lib, st):
    assert st.unlocked == 0 and st.sleep_pending == 0


def test_sleep_without_unlock_is_noop(lib, st):
    assert feed(lib, st, A6, SUB_SLEEP) == NONE
    assert st.sleep_pending == 0


def test_unlock_then_sleep_arms(lib, st):
    assert feed(lib, st, A6, SUB_UNLOCK) == SEND_READY
    assert st.unlocked == 1
    assert feed(lib, st, A6, SUB_SLEEP) == ARM
    assert st.sleep_pending == 1


def test_unlock_persists_across_transport(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, 0x30)          # select 2.4G
    feed(lib, st, A6, 0x52)          # unpair
    assert st.unlocked == 1
    assert feed(lib, st, A6, SUB_SLEEP) == ARM


def test_transport_after_sleep_cancels(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_SLEEP)
    assert feed(lib, st, A6, 0x30) == CANCEL
    assert st.sleep_pending == 0


def test_ota_cancels_and_wins(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_SLEEP)
    assert feed(lib, st, A6, 0x81) == CANCEL
    assert st.sleep_pending == 0
    # A6 54 while OTA is pending must not re-arm.
    assert feed(lib, st, A6, SUB_SLEEP, ob=1) == NONE
    assert st.sleep_pending == 0


def test_hid_frame_cancels_pending(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_SLEEP)
    assert feed(lib, st, A1) == CANCEL
    assert st.sleep_pending == 0


def test_duplicate_sleep_keeps_request(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    assert feed(lib, st, A6, SUB_SLEEP) == ARM
    assert feed(lib, st, A6, SUB_SLEEP) == NONE   # dup does not re-arm/cancel
    assert st.sleep_pending == 1


def test_host_ack_is_inert(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_SLEEP)
    assert feed(lib, st, ACK, 0x0D) == NONE
    assert st.sleep_pending == 1        # ACK must not cancel


def test_unlock_re_request_cancels_pending(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_SLEEP)
    assert feed(lib, st, A6, SUB_UNLOCK) == SEND_READY
    assert st.sleep_pending == 0        # re-negotiate stands the sleep down
    assert st.unlocked == 1


@pytest.mark.parametrize("sub", [0xFF, 0xAB, 0x00])
def test_unrecognised_a6_sub_is_inert(lib, st, sub):
    # An accepted but unrecognised A6 subcommand must NOT defeat a requested
    # sleep (contract: unrecognised frames are inert).
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_SLEEP)
    assert feed(lib, st, A6, sub) == NONE
    assert st.sleep_pending == 1


@pytest.mark.parametrize("sub", [0x53, 0x70])
def test_query_commands_do_not_cancel_sleep(lib, st, sub):
    # Battery/version queries are not "changed my mind" -- the sleep proceeds.
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_SLEEP)
    assert feed(lib, st, A6, sub) == NONE
    assert st.sleep_pending == 1


@pytest.mark.parametrize("sub", [0x11, 0x30, 0x31, 0x32, 0x33, 0x51, 0x52, 0x63, 0x81])
def test_state_changing_commands_cancel_sleep(lib, st, sub):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_SLEEP)
    assert feed(lib, st, A6, sub) == CANCEL
    assert st.sleep_pending == 0


def test_hid_without_pending_is_inert(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    assert feed(lib, st, A1) == NONE
    assert st.unlocked == 1


def test_name_frame_cancels_pending(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_SLEEP)
    assert feed(lib, st, A9, 0x05) == CANCEL   # A9 name frame, arbitrary sub
    assert st.sleep_pending == 0


@pytest.mark.parametrize("cmd", [A1, A9])
@pytest.mark.parametrize("sub", [0x00, SUB_SLEEP, SUB_UNLOCK])
def test_non_a6_sub_never_unlocks_or_arms(lib, st, cmd, sub):
    # The reducer must key on cmd==A6 before interpreting sub: an A1/A9 frame
    # whose second byte happens to be 0x54/0x56 must not unlock or arm.
    act = feed(lib, st, cmd, sub)
    assert act == NONE
    assert st.unlocked == 0 and st.sleep_pending == 0


@pytest.mark.parametrize("sub", [0x55, 0x57])
def test_reserved_sleep_subs_are_inert(lib, st, sub):
    # A6 55 (BT twin) and A6 57 (MR7 auto-sleep) must not arm explicit sleep,
    # and -- as reserved sleep-family subs -- must not cancel a pending one.
    feed(lib, st, A6, SUB_UNLOCK)
    assert feed(lib, st, A6, sub) == NONE
    assert st.sleep_pending == 0
    feed(lib, st, A6, SUB_SLEEP)                # now pending
    assert feed(lib, st, A6, sub) == NONE       # reserved: inert, sleep stands
    assert st.sleep_pending == 1


# ---------------- MR7: A6 57 auto-sleep lifetime ----------------

def test_reset_clears_autosleep(lib, st):
    assert st.autosleep == 0


def test_autosleep_without_unlock_is_noop(lib, st):
    assert feed(lib, st, A6, SUB_AUTO) == NONE
    assert st.autosleep == 0


def test_unlock_then_57_arms_autosleep(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    assert feed(lib, st, A6, SUB_AUTO) == NONE      # ACK only; no new status
    assert st.autosleep == 1
    assert feed(lib, st, A6, SUB_AUTO) == NONE      # idempotent
    assert st.autosleep == 1


def test_reunlock_disables_autosleep(lib, st):
    # A6 56 = reset to baseline: the disable path (stock has no disable opcode).
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_AUTO)
    assert feed(lib, st, A6, SUB_UNLOCK) == SEND_READY
    assert st.autosleep == 0 and st.unlocked == 1


@pytest.mark.parametrize("sub", [0x51, 0x52, 0x63])
def test_pairing_and_unpair_clear_autosleep(lib, st, sub):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_AUTO)
    feed(lib, st, A6, sub)
    assert st.autosleep == 0
    assert st.unlocked == 1                        # unlock itself survives


@pytest.mark.parametrize("sub", [0x11, 0x30, 0x31, 0x32, 0x33, 0x53, 0x70, 0xFF])
def test_transport_and_queries_preserve_autosleep(lib, st, sub):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_AUTO)
    feed(lib, st, A6, sub)
    assert st.autosleep == 1


@pytest.mark.parametrize("cmd", [A1, A9, ACK])
def test_non_a6_frames_preserve_autosleep(lib, st, cmd):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_AUTO)
    feed(lib, st, cmd, SUB_AUTO)                   # even with sub byte 0x57
    assert st.autosleep == 1


def test_57_does_not_touch_pending_explicit_sleep(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_SLEEP)
    assert feed(lib, st, A6, SUB_AUTO) == NONE
    assert st.sleep_pending == 1 and st.autosleep == 1


def test_explicit_sleep_and_autosleep_are_independent(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_AUTO)
    assert feed(lib, st, A6, SUB_SLEEP) == ARM      # explicit still arms
    assert feed(lib, st, A6, 0x30) == CANCEL        # cancels explicit only
    assert st.autosleep == 1


def test_ota_preserves_autosleep_until_reboot(lib, st):
    feed(lib, st, A6, SUB_UNLOCK)
    feed(lib, st, A6, SUB_AUTO)
    feed(lib, st, A6, 0x81)                        # OTA: module reboots anyway
    assert st.autosleep == 1


# ---------------- MR7: backstep-tolerant elapsed ticks ----------------
MOD, TOL = 0xA8C00000, 1024


def elapsed(lib, now, start):
    return lib.SleepProtocol_ElapsedTicks(now, start, MOD, TOL)


def test_elapsed_normal(lib):
    assert elapsed(lib, 5000, 1000) == 4000
    assert elapsed(lib, 1000, 1000) == 0


@pytest.mark.parametrize("back", [1, 100, 1024])
def test_elapsed_small_backstep_is_zero(lib, back):
    # The CH59x RTC32K read can step backward a few ticks: NOT a wrap.
    assert elapsed(lib, 100000 - back, 100000) == 0


def test_elapsed_true_wrap(lib):
    # start near the modulus, now just past zero: a genuine wrap.
    assert elapsed(lib, 10, MOD - 5) == 15
    # just beyond the tolerance is treated as a wrap, not a backstep
    assert elapsed(lib, 100000 - TOL - 1, 100000) == MOD - TOL - 1


@pytest.mark.parametrize("cmd,sub,expect", [
    (A6, 0x11, 1), (A6, 0x30, 1), (A6, 0x33, 1), (A6, 0x51, 1), (A6, 0x52, 1), (A6, 0x63, 1), (A6, 0x81, 1),
    (A1, 0x00, 1), (A9, 0x05, 1),
    (A6, 0x53, 0), (A6, 0x70, 0), (A6, SUB_SLEEP, 0), (A6, SUB_UNLOCK, 0), (A6, SUB_AUTO, 0), (A6, 0xFF, 0), (ACK, 0x0D, 0),
])
def test_is_state_changing_matches_cancel_set(lib, cmd, sub, expect):
    # The firmware uses this to withdraw an explicit sleep request already
    # handed to the power layer; it must agree with the reducer's cancel set.
    assert lib.SleepProtocol_IsStateChanging(cmd, sub) == expect
