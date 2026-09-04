"""UART diag-dump frame formatter tests (pure module, native-compiled)."""
import ctypes, struct, subprocess
from pathlib import Path
import pytest

FW = Path(__file__).resolve().parent.parent
PAYLOAD_LEN, HEADER = 65, 0x5D
FMT = "<BBIIIIIIIHHHHHBBBBIBBIIIHB"     # matches diag_frame.h v2 layout
FIELDS = ["version","rf_state","ll_boot_count","rf_pair_bcast_count","rf_valid_rx_count",
          "entered_connected_count","rf_config_count","pwr_pair_rx_off_count","pwr_wfi_count",
          "pwr_sleep_attempt","pwr_sleep_entered","pwr_sleep_aborted","pwr_wake_gpio","pwr_wake_rtc",
          "pwr_last_abort_reason","rf_last_config_status","rf_last_rx_status","rf_last_tx_status",
          "ll_drop_count","boot_reset_status","fault_marker","fault_mepc","fault_mcause","fault_mtval","pwr_loop_passes","pwr_loop_stage"]

class Snap(ctypes.Structure):
    _fields_ = [("rf_state", ctypes.c_uint8),
                ("ll_boot_count", ctypes.c_uint32), ("rf_pair_bcast_count", ctypes.c_uint32),
                ("rf_valid_rx_count", ctypes.c_uint32), ("entered_connected_count", ctypes.c_uint32),
                ("rf_config_count", ctypes.c_uint32), ("pwr_pair_rx_off_count", ctypes.c_uint32),
                ("pwr_wfi_count", ctypes.c_uint32),
                ("pwr_sleep_attempt", ctypes.c_uint16), ("pwr_sleep_entered", ctypes.c_uint16),
                ("pwr_sleep_aborted", ctypes.c_uint16), ("pwr_wake_gpio", ctypes.c_uint16),
                ("pwr_wake_rtc", ctypes.c_uint16),
                ("pwr_last_abort_reason", ctypes.c_uint8), ("rf_last_config_status", ctypes.c_uint8),
                ("rf_last_rx_status", ctypes.c_uint8), ("rf_last_tx_status", ctypes.c_uint8),
                ("ll_drop_count", ctypes.c_uint32),
                ("boot_reset_status", ctypes.c_uint8), ("fault_marker", ctypes.c_uint8),
                ("fault_mepc", ctypes.c_uint32), ("fault_mcause", ctypes.c_uint32), ("fault_mtval", ctypes.c_uint32),
                ("pwr_loop_passes", ctypes.c_uint16), ("pwr_loop_stage", ctypes.c_uint8)]

@pytest.fixture(scope="module")
def lib(tmp_path_factory):
    so = tmp_path_factory.mktemp("diag") / "diag_frame.so"
    subprocess.run(["cc","-shared","-fPIC","-O1","-I",str(FW/"src"),str(FW/"src"/"diag_frame.c"),"-o",str(so)],check=True)
    d = ctypes.CDLL(str(so))
    d.DiagFrame_Format.restype = ctypes.c_uint8
    d.DiagFrame_Format.argtypes = [ctypes.c_char_p, ctypes.POINTER(Snap)]
    d.DiagFrame_FormatEmpty.restype = ctypes.c_uint8
    d.DiagFrame_FormatEmpty.argtypes = [ctypes.c_char_p]
    return d

def decode(frame: bytes) -> dict:
    assert frame[0] == HEADER
    n = frame[1]; assert len(frame) == n + 3
    assert frame[-1] == sum(frame[:-1]) & 0xFF, "checksum"
    assert struct.calcsize(FMT) == PAYLOAD_LEN == n
    return dict(zip(FIELDS, struct.unpack(FMT, frame[2:-1])))

def test_layout_and_checksum(lib):
    # Every field gets a distinct sentinel so any same-width field swap is caught.
    vals = {}
    for idx, name in enumerate(FIELDS):
        w = {"B": 0xFF, "H": 0xFFFF, "I": 0xFFFFFFFF}[FMT[1 + idx]]
        vals[name] = (idx * 0x01010101 + 0x0F) & w
    vals["version"] = 2
    snap = Snap(**{k: v for k, v in vals.items() if k != "version"})
    buf = ctypes.create_string_buffer(80)
    n = lib.DiagFrame_Format(buf, ctypes.byref(snap))
    assert n == PAYLOAD_LEN + 3
    assert decode(buf.raw[:n]) == vals

def test_empty_frame(lib):
    buf = ctypes.create_string_buffer(8)
    n = lib.DiagFrame_FormatEmpty(buf)
    assert buf.raw[:n] == bytes([0x5D, 0x00, 0x5D])

def test_checksum_covers_header_and_len(lib):
    snap = Snap()   # all zero
    buf = ctypes.create_string_buffer(80)
    n = lib.DiagFrame_Format(buf, ctypes.byref(snap))
    f = buf.raw[:n]
    assert f[-1] == (0x5D + PAYLOAD_LEN + 2) & 0xFF   # version=2 is the only nonzero payload byte
