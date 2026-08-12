"""Application board-profile configuration tests.

These use the toolchain-free print target.  Besides checking the hardware
knobs, they ensure similarly sized CH592 products cannot share build or
release artifact paths.
"""
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


@pytest.mark.parametrize("board,remap,factory_mac,openboot_board", [
    ("opencontroller-ch592", "1", "0", "opencontroller-ch592"),
    ("mk65mx-wireless-ch592", "0", "1", "mk65mx-wireless-ch592"),
])
def test_board_profile(board, remap, factory_mac, openboot_board):
    cfg = board_config(board)
    assert cfg["BOARD"] == board
    assert cfg["OPENBOOT_BOARD"] == openboot_board
    assert cfg["KBD_UART1_REMAP"] == remap
    assert cfg["KBD_FACTORY_MAC"] == factory_mac
    assert board in cfg["BUILD"]
    assert board in cfg["BUNDLE_BIN"]
    assert board in cfg["FACTORY_BIN"]


def test_board_artifact_paths_do_not_overlap():
    original = board_config("opencontroller-ch592")
    mk65 = board_config("mk65mx-wireless-ch592")
    for key in ("BUILD", "BUNDLE_BIN", "FACTORY_BIN"):
        assert original[key] != mk65[key]
