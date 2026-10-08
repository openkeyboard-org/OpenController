"""Application board-profile configuration tests.

These use the toolchain-free print target.  Besides checking the hardware
knobs, they ensure similarly sized CH592 products cannot share build or
release artifact paths.
"""
import re
import subprocess
from pathlib import Path

import pytest

FW = Path(__file__).resolve().parent.parent


def board_config(board: str) -> dict[str, str]:
    result = subprocess.run(
        ["make", "--no-print-directory", "-s", "-C", str(FW),
         f"BOARD={board}", "print-board-config"],
        check=True, capture_output=True, text=True)
    return dict(line.split("=", 1) for line in result.stdout.splitlines())


@pytest.mark.parametrize("board,remap,factory_mac,dcdc,deep_sleep,host_wake,openboot_board", [
    ("opencontroller-ch592", "1", "0", "1", "1", "0", "opencontroller-ch592"),
    ("mk65mx-wireless-ch592", "0", "1", "1", "1", "1", "mk65mx-wireless-ch592"),
])
def test_board_profile(board, remap, factory_mac, dcdc, deep_sleep, host_wake, openboot_board):
    cfg = board_config(board)
    assert cfg["BOARD"] == board
    assert cfg["OPENBOOT_BOARD"] == openboot_board
    assert cfg["KBD_UART1_REMAP"] == remap
    assert cfg["KBD_FACTORY_MAC"] == factory_mac
    assert cfg["KBD_DCDC_ENABLE"] == dcdc
    assert cfg["KBD_DEEP_SLEEP"] == deep_sleep
    assert cfg["KBD_HOST_WAKE"] == host_wake
    assert board in cfg["BUILD"]
    assert board in cfg["BUNDLE_BIN"]
    assert board in cfg["FACTORY_BIN"]


def test_board_artifact_paths_do_not_overlap():
    original = board_config("opencontroller-ch592")
    mk65 = board_config("mk65mx-wireless-ch592")
    for key in ("BUILD", "BUNDLE_BIN", "FACTORY_BIN"):
        assert original[key] != mk65[key]


@pytest.mark.parametrize("bad", ["2", "yes", "0 1", ""])
def test_dcdc_enable_must_be_an_exact_boolean(bad):
    """A typo must not silently fall back to the LDO, or worse, to enabling
    the converter on a board that has no inductor."""
    result = subprocess.run(
        ["make", "--no-print-directory", "-s", "-C", str(FW),
         "BOARD=mk65mx-wireless-ch592", f"KBD_DCDC_ENABLE={bad}",
         "print-board-config"],
        capture_output=True, text=True)

    assert result.returncode != 0
    assert "KBD_DCDC_ENABLE" in result.stderr


def test_dcdc_enable_rejected_in_extra_cflags():
    """A -D in EXTRA_CFLAGS would win the CFLAGS redefinition silently while
    print-board-config keeps reporting the board value; the deliberate
    override path is `make KBD_DCDC_ENABLE=...`, which stays validated."""
    result = subprocess.run(
        ["make", "--no-print-directory", "-s", "-C", str(FW),
         "EXTRA_CFLAGS=-DKBD_DCDC_ENABLE=0", "print-board-config"],
        capture_output=True, text=True)

    assert result.returncode != 0
    assert "EXTRA_CFLAGS" in result.stderr


@pytest.mark.parametrize("bad", ["2", "yes", "0 1", ""])
def test_deep_sleep_must_be_an_exact_boolean(bad):
    """Mirrors the KBD_DCDC_ENABLE validation: a typo must fail the build,
    never silently pick a sleep configuration."""
    result = subprocess.run(
        ["make", "--no-print-directory", "-s", "-C", str(FW),
         f"KBD_DEEP_SLEEP={bad}", "print-board-config"],
        capture_output=True, text=True)

    assert result.returncode != 0
    assert "KBD_DEEP_SLEEP" in result.stderr


@pytest.mark.parametrize("bad", ["2", "yes", "0 1", ""])
def test_host_wake_must_be_an_exact_boolean(bad):
    result = subprocess.run(
        ["make", "--no-print-directory", "-s", "-C", str(FW),
         "BOARD=mk65mx-wireless-ch592", f"KBD_HOST_WAKE={bad}",
         "print-board-config"],
        capture_output=True, text=True)

    assert result.returncode != 0
    assert "KBD_HOST_WAKE" in result.stderr


def test_host_wake_refuses_the_uart1_remap():
    """CHWAKE is PB13, which the remap uses as TXD1: driving it would fight
    the UART."""
    result = subprocess.run(
        ["make", "--no-print-directory", "-s", "-C", str(FW),
         "BOARD=opencontroller-ch592", "KBD_HOST_WAKE=1", "print-board-config"],
        capture_output=True, text=True)

    assert result.returncode != 0
    assert "PB13" in result.stderr


@pytest.mark.parametrize("extra", [
    "-DKBD_HOST_WAKE=0", "-DKBD_HOST_WAKE", "-D KBD_HOST_WAKE=0",
    "-D KBD_HOST_WAKE", "-UKBD_HOST_WAKE", "-U KBD_HOST_WAKE",
])
def test_host_wake_rejected_in_extra_cflags_in_every_spelling(extra):
    rejected = subprocess.run(
        ["make", "--no-print-directory", "-s", "-C", str(FW),
         "BOARD=mk65mx-wireless-ch592", f"EXTRA_CFLAGS={extra}",
         "print-board-config"],
        capture_output=True, text=True)
    assert rejected.returncode != 0
    assert "KBD_HOST_WAKE" in rejected.stderr


def test_host_wake_timing_stays_tunable_in_extra_cflags():

    tuned = subprocess.run(
        ["make", "--no-print-directory", "-s", "-C", str(FW),
         "BOARD=mk65mx-wireless-ch592", "EXTRA_CFLAGS=-DKBD_HOST_WAKE_GUARD_US=500",
         "print-board-config"],
        capture_output=True, text=True)
    assert tuned.returncode == 0, tuned.stderr


@pytest.mark.parametrize("macro", ["KBD_DEEP_SLEEP", "HAL_SLEEP"])
def test_sleep_macros_rejected_in_extra_cflags(macro):
    """Both macros must reach every SDK source coherently; an EXTRA_CFLAGS
    -D would win the redefinition silently (KBD_DEEP_SLEEP) or split the
    build between app and SDK CONFIG.h (HAL_SLEEP)."""
    result = subprocess.run(
        ["make", "--no-print-directory", "-s", "-C", str(FW),
         f"EXTRA_CFLAGS=-D{macro}=1", "print-board-config"],
        capture_output=True, text=True)

    assert result.returncode != 0
    assert macro in result.stderr


def _print_config(*make_args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["make", "--no-print-directory", "-s", "-C", str(FW), *make_args, "print-board-config"],
        capture_output=True, text=True)


@pytest.mark.parametrize("bad", ["abc", "3.5", "0 1", "", "499", "60001", "-1", "+3000", "0xBB8", "3000''"])
def test_rest_idle_ms_must_be_a_whole_number_in_range(bad):
    """T_idle is validated like the other knobs: one whole number of ms in
    500..60000; anything else must fail loudly, not silently build a policy."""
    result = _print_config(f"KBD_REST_IDLE_MS={bad}")

    assert result.returncode != 0
    assert "KBD_REST_IDLE_MS" in result.stderr


@pytest.mark.parametrize("extra", ["-DKBD_REST_IDLE_TICKS=8000u", "-DKBD_REST_IDLE_MS=5000"])
def test_rest_idle_macros_rejected_in_extra_cflags(extra):
    """Both macros are derived from KBD_REST_IDLE_MS in one place; a -D in
    EXTRA_CFLAGS would win the redefinition silently while print-board-config
    keeps reporting the ms value."""
    result = _print_config(f"EXTRA_CFLAGS={extra}")

    assert result.returncode != 0
    assert "KBD_REST_IDLE" in result.stderr


def test_rest_idle_ticks_not_settable_on_the_command_line():
    """A tick count given directly would skip the ms range check."""
    result = _print_config("KBD_REST_IDLE_TICKS=0")

    assert result.returncode != 0
    assert "KBD_REST_IDLE_TICKS" in result.stderr


@pytest.mark.parametrize("ms,ticks", [("3000", "4800"), ("5000", "8000"), ("2000", "3200"), ("500", "800"), ("60000", "96000"), ("3001", "4801")])
def test_rest_idle_ms_converts_to_tmos_ticks(ms, ticks):
    """625 us TMOS units: ms x 8/5. The C default (4800u) must equal the
    3000 ms knob default so a build without the knob and one with it agree."""
    result = _print_config(f"KBD_REST_IDLE_MS={ms}")

    assert result.returncode == 0, result.stderr
    config = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    assert config["KBD_REST_IDLE_MS"] == ms
    assert config["KBD_REST_IDLE_TICKS"] == ticks


def test_rest_idle_default_matches_c_default():
    """The Makefile default and the #ifndef fallback in rf_task.c are the same
    number, so a build that bypasses the knob cannot land a different policy."""
    result = _print_config()
    assert result.returncode == 0, result.stderr
    config = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    source = (FW / "src" / "rf_task.c").read_text()
    fallback = re.search(r"^#define KBD_REST_IDLE_TICKS\s+(\S+)", source, re.M).group(1)
    assert fallback == config["KBD_REST_IDLE_TICKS"] + "u" == "4800u"
