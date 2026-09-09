"""Source-level regression checks for the RF HID-report delivery contract.

The RF task is tightly coupled to WCH's interrupt and radio SDK, so it is not
currently linkable as a host unit.  This small check still guards the critical
property: every UART submission, including an identical empty release barrier,
must unconditionally re-arm the RF resend budget.
"""
from pathlib import Path


RF_TASK = Path(__file__).resolve().parent.parent / "src" / "rf_task.c"


def test_every_hid_submission_rearms_rf_resends():
    source = RF_TASK.read_text()
    body = source.split("void RF_QueueHIDReport(const uint8_t report[8])", 1)[1]
    body = body.split("uint8_t RF_GetState(void)", 1)[0]

    assert "hid_report[i] = report[i];" in body
    assert body.count("hid_resend = HID_RESEND_COUNT;") == 1
    assert "changed" not in body
