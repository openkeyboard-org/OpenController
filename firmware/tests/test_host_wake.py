"""Host wake (CHWAKE) state machine tests.

Native-compiles the HAL-free src/host_wake.c with the host compiler and drives
it through ctypes: guard time, ACK accounting, retries, quarantine after a
resend, the LED latest-value slot, and the queue bound, without hardware.
"""
import ctypes
import subprocess
from pathlib import Path

import pytest

FW = Path(__file__).resolve().parent.parent
SRC = FW / "src" / "host_wake.c"

QUEUE = 8          # HOST_WAKE_QUEUE_SIZE
GUARD = 1000       # ticks; 1 tick = 1 us here
TIMEOUT = 20000
ATTEMPTS = 3


class HostWake(ctypes.Structure):
    _fields_ = [
        ("guard_ticks", ctypes.c_uint32),
        ("ack_timeout_ticks", ctypes.c_uint32),
        ("max_attempts", ctypes.c_uint8),
        ("state", ctypes.c_uint8),
        ("head", ctypes.c_uint8),
        ("count", ctypes.c_uint8),
        ("attempts", ctypes.c_uint8),
        ("acked", ctypes.c_uint8),
        ("sending_latest", ctypes.c_uint8),
        ("last_latest", ctypes.c_uint8),
        ("since", ctypes.c_uint32),
        ("cmd", ctypes.c_uint8 * QUEUE),
        ("val", ctypes.c_uint8 * QUEUE),
        ("latest_pending", ctypes.c_uint8),
        ("latest_lost", ctypes.c_uint8),
        ("latest_cmd", ctypes.c_uint8),
        ("latest_val", ctypes.c_uint8),
        ("out_cmd", ctypes.c_uint8),
        ("out_val", ctypes.c_uint8),
        ("dropped", ctypes.c_uint16),
        ("abandoned", ctypes.c_uint16),
        ("retired", ctypes.c_uint16),
    ]


class Step(ctypes.Structure):
    _fields_ = [("line", ctypes.c_uint8), ("send", ctypes.c_uint8),
                ("cmd", ctypes.c_uint8), ("val", ctypes.c_uint8)]


@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    so = tmp_path_factory.mktemp("hostwake") / "host_wake.so"
    subprocess.run(
        ["cc", "-shared", "-fPIC", "-O1", "-Wall", "-Werror", "-I", str(FW / "src"),
         str(SRC), "-o", str(so)],
        check=True)
    dll = ctypes.CDLL(str(so))
    dll.HostWake_Init.argtypes = [ctypes.POINTER(HostWake), ctypes.c_uint32,
                                  ctypes.c_uint32, ctypes.c_uint8]
    dll.HostWake_Enqueue.restype = ctypes.c_uint8
    dll.HostWake_Enqueue.argtypes = [ctypes.POINTER(HostWake), ctypes.c_uint8,
                                     ctypes.c_uint8]
    dll.HostWake_SetLatest.argtypes = [ctypes.POINTER(HostWake), ctypes.c_uint8,
                                       ctypes.c_uint8]
    dll.HostWake_OnAck.argtypes = [ctypes.POINTER(HostWake)]
    dll.HostWake_Step.restype = Step
    dll.HostWake_Step.argtypes = [ctypes.POINTER(HostWake), ctypes.c_uint32]
    dll.HostWake_Idle.restype = ctypes.c_uint8
    dll.HostWake_Idle.argtypes = [ctypes.POINTER(HostWake)]
    return dll


@pytest.fixture
def hw(lib):
    h = HostWake()
    lib.HostWake_Init(ctypes.byref(h), GUARD, TIMEOUT, ATTEMPTS)
    return h


def step(lib, hw, now):
    s = lib.HostWake_Step(ctypes.byref(hw), now & 0xFFFFFFFF)
    return s.line, (s.cmd, s.val) if s.send else None


def enqueue(lib, hw, cmd, val):
    return lib.HostWake_Enqueue(ctypes.byref(hw), cmd, val)


def set_led(lib, hw, val):
    lib.HostWake_SetLatest(ctypes.byref(hw), 0x5A, val)


def ack(lib, hw):
    lib.HostWake_OnAck(ctypes.byref(hw))


def idle(lib, hw):
    return lib.HostWake_Idle(ctypes.byref(hw))


def test_idle_keeps_the_line_low(lib, hw):
    assert step(lib, hw, 0) == (0, None)
    assert idle(lib, hw)


def test_frame_waits_out_the_guard_then_goes_out_once(lib, hw):
    assert enqueue(lib, hw, 0x5B, 0x32)
    assert not idle(lib, hw)
    assert step(lib, hw, 100) == (1, None)                # line up, guard starts
    assert step(lib, hw, 100 + GUARD - 1) == (1, None)
    assert step(lib, hw, 100 + GUARD) == (1, (0x5B, 0x32))
    assert step(lib, hw, 100 + GUARD + 10) == (1, None)   # waiting for the ACK
    ack(lib, hw)
    assert step(lib, hw, 100 + GUARD + 20) == (0, None)   # retired, line down
    assert idle(lib, hw)


def test_a_burst_pays_the_guard_once(lib, hw):
    for sub in (0x34, 0x35):
        assert enqueue(lib, hw, 0x5B, sub)
    assert step(lib, hw, 0) == (1, None)
    assert step(lib, hw, GUARD) == (1, (0x5B, 0x34))
    ack(lib, hw)
    # Acked on its only copy: the next frame follows at once, line still up.
    assert step(lib, hw, GUARD + 50) == (1, (0x5B, 0x35))
    ack(lib, hw)
    assert step(lib, hw, GUARD + 100) == (0, None)
    assert idle(lib, hw)


def test_missing_ack_is_retried_then_abandoned_then_quarantined(lib, hw):
    assert enqueue(lib, hw, 0x5B, 0x21)
    assert enqueue(lib, hw, 0x5B, 0x23)
    step(lib, hw, 0)
    t = GUARD
    assert step(lib, hw, t) == (1, (0x5B, 0x21))
    for _ in range(ATTEMPTS - 1):
        assert step(lib, hw, t + TIMEOUT - 1) == (1, None)
        t += TIMEOUT
        assert step(lib, hw, t) == (1, (0x5B, 0x21))     # resend
    # Out of attempts: abandoned, then quarantine until two ACK timeouts
    # after the last copy (sent at t) before the next frame.
    assert step(lib, hw, t + TIMEOUT) == (1, None)
    assert hw.abandoned == 1
    assert step(lib, hw, t + 2 * TIMEOUT - 1) == (1, None)
    assert step(lib, hw, t + 2 * TIMEOUT) == (1, (0x5B, 0x23))
    ack(lib, hw)
    assert step(lib, hw, t + 2 * TIMEOUT + 1) == (0, None)


def test_a_late_ack_after_a_resend_cannot_retire_the_next_frame(lib, hw):
    """Codex's scenario: A's ACK is late, A is resent, the first ACK retires A;
    the second ACK for A must not retire B."""
    assert enqueue(lib, hw, 0x5B, 0x0A)
    assert enqueue(lib, hw, 0x5B, 0x0B)
    step(lib, hw, 0)
    a0 = GUARD
    assert step(lib, hw, a0) == (1, (0x5B, 0x0A))
    resend = a0 + TIMEOUT
    assert step(lib, hw, resend) == (1, (0x5B, 0x0A))           # copy 2
    ack(lib, hw)                                                 # ACK of copy 1
    assert step(lib, hw, resend + 1000) == (1, None)             # A retired, quarantine
    # Codex's timing: copy 2's ACK arrives 35 ms after the resend, inside the
    # two-timeout bound. It must be discarded, not retire B.
    assert step(lib, hw, resend + 35000 - 1) == (1, None)
    ack(lib, hw)
    assert step(lib, hw, resend + 35000) == (1, None)
    assert step(lib, hw, resend + 2 * TIMEOUT - 1) == (1, None)
    assert step(lib, hw, resend + 2 * TIMEOUT) == (1, (0x5B, 0x0B))  # B afresh
    assert step(lib, hw, resend + 2 * TIMEOUT + 1) == (1, None)      # B outstanding
    ack(lib, hw)
    assert step(lib, hw, resend + 2 * TIMEOUT + 2) == (0, None)
    assert hw.retired == 2 and hw.abandoned == 0


def test_an_ack_with_nothing_outstanding_is_ignored(lib, hw):
    ack(lib, hw)                                         # idle: answers nothing
    assert enqueue(lib, hw, 0x5B, 0x38)
    step(lib, hw, 0)
    ack(lib, hw)                                         # during the guard
    assert step(lib, hw, GUARD) == (1, (0x5B, 0x38))
    assert step(lib, hw, GUARD + 1) == (1, None)
    ack(lib, hw)
    assert step(lib, hw, GUARD + 2) == (0, None)


def test_the_led_slot_keeps_only_the_newest_value_and_goes_first(lib, hw):
    set_led(lib, hw, 0x01)
    assert enqueue(lib, hw, 0x5B, 0x32)
    set_led(lib, hw, 0x02)                               # replaces 0x01 unsent
    step(lib, hw, 0)
    assert step(lib, hw, GUARD) == (1, (0x5A, 0x02))     # LED ahead of the queue
    ack(lib, hw)
    assert step(lib, hw, GUARD + 1) == (1, (0x5B, 0x32))
    ack(lib, hw)
    assert step(lib, hw, GUARD + 2) == (0, None)
    assert idle(lib, hw)


def test_steady_status_traffic_cannot_starve_the_led(lib, hw):
    assert enqueue(lib, hw, 0x5B, 0x01)
    step(lib, hw, 0)
    assert step(lib, hw, GUARD) == (1, (0x5B, 0x01))
    set_led(lib, hw, 0x02)                               # arrives mid-burst
    assert enqueue(lib, hw, 0x5B, 0x02)
    ack(lib, hw)
    assert step(lib, hw, GUARD + 1) == (1, (0x5A, 0x02))  # before the next status


def test_a_re_offered_led_value_cannot_starve_the_queue(lib, hw):
    """A host that stops ACKing while the receiver keeps reporting LEDs:
    LED and queued frames must take turns."""
    assert enqueue(lib, hw, 0x5B, 0x01)
    assert enqueue(lib, hw, 0x5B, 0x02)
    set_led(lib, hw, 0x02)
    step(lib, hw, 0)
    sent = []
    t = GUARD
    for n in range(4):
        out = step(lib, hw, t)[1]
        sent.append(out)
        set_led(lib, hw, 0x02)                           # re-offered every time
        ack(lib, hw)
        t += 1
    assert sent == [(0x5A, 0x02), (0x5B, 0x01), (0x5A, 0x02), (0x5B, 0x02)]


def test_an_abandoned_led_value_is_reported_lost(lib, hw):
    set_led(lib, hw, 0x02)
    step(lib, hw, 0)
    t = GUARD
    assert step(lib, hw, t) == (1, (0x5A, 0x02))
    for _ in range(ATTEMPTS - 1):
        t += TIMEOUT
        assert step(lib, hw, t) == (1, (0x5A, 0x02))
    assert step(lib, hw, t + TIMEOUT) == (1, None)        # abandoned
    assert hw.latest_lost == 1


def test_an_abandoned_led_value_with_a_newer_one_behind_is_not_lost(lib, hw):
    set_led(lib, hw, 0x02)
    step(lib, hw, 0)
    t = GUARD
    assert step(lib, hw, t) == (1, (0x5A, 0x02))
    set_led(lib, hw, 0x00)                               # newer value waiting
    for _ in range(ATTEMPTS - 1):
        t += TIMEOUT
        assert step(lib, hw, t) == (1, (0x5A, 0x02))
    assert step(lib, hw, t + TIMEOUT) == (1, None)
    assert hw.latest_lost == 0
    assert step(lib, hw, t + 2 * TIMEOUT) == (1, (0x5A, 0x00))


def test_a_led_change_while_its_value_is_on_the_wire_is_sent_after_it(lib, hw):
    set_led(lib, hw, 0x02)
    step(lib, hw, 0)
    assert step(lib, hw, GUARD) == (1, (0x5A, 0x02))
    set_led(lib, hw, 0x00)                               # Caps Lock off meanwhile
    ack(lib, hw)
    assert step(lib, hw, GUARD + 1) == (1, (0x5A, 0x00))
    ack(lib, hw)
    assert step(lib, hw, GUARD + 2) == (0, None)


def test_a_full_queue_never_loses_the_led_state(lib, hw):
    for i in range(QUEUE):
        assert enqueue(lib, hw, 0x5B, i)
    assert not enqueue(lib, hw, 0x5B, 0xFF)
    assert hw.dropped == 1
    set_led(lib, hw, 0x02)
    step(lib, hw, 0)
    sent = [step(lib, hw, GUARD)[1]]
    for n in range(1, QUEUE + 1):
        ack(lib, hw)
        sent.append(step(lib, hw, GUARD + n)[1])
    assert sent == [(0x5A, 0x02)] + [(0x5B, i) for i in range(QUEUE)]
    ack(lib, hw)
    assert step(lib, hw, GUARD + QUEUE + 1) == (0, None)


def test_timing_is_wrap_safe(lib, hw):
    start = 0xFFFFFF00
    assert enqueue(lib, hw, 0x5B, 0x32)
    assert step(lib, hw, start) == (1, None)
    assert step(lib, hw, start + GUARD - 1) == (1, None)
    assert step(lib, hw, start + GUARD) == (1, (0x5B, 0x32))
    # The ACK timeout and a resend across the wrap too.
    assert step(lib, hw, start + GUARD + TIMEOUT) == (1, (0x5B, 0x32))


def test_a_frame_queued_while_idle_raises_the_line_on_the_next_step(lib, hw):
    assert step(lib, hw, 0) == (0, None)
    assert enqueue(lib, hw, 0x5C, 100)
    assert step(lib, hw, 5) == (1, None)
    assert step(lib, hw, 5 + GUARD) == (1, (0x5C, 100))
