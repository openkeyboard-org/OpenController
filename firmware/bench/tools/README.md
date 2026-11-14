# Bench tools: report-delivery observability

Host-side tooling for the report-delivery work (see the plan in the PR that adds
it). These do not touch firmware; they measure what the firmware delivers.

## `hid_capture.py` -- per-report keystroke oracle

The earlier bench oracle was the macOS HID idle timer, which only reveals that
*some* input arrived. It cannot identify the key, tell a press from a release,
or preserve order, so it cannot verify "every press and release reached the host
in order" -- the actual delivery gate.

`hid_capture.py` installs a listen-only `CGEventTap` and logs every keyDown /
keyUp / modifier change for a chosen set of keys, each with a monotonic
millisecond timestamp, as NDJSON. A test injects a key the human is not pressing
(F13/F24, or a bare modifier), so every event of that key during the window came
from the device under test -- the same attribution the SWD/host injector already
uses.

```
# validate the tap plumbing with a synthetic event (no hardware):
python hid_capture.py selftest            # -> selftest: PASS

# capture the test keys to a file while a run injects taps:
python hid_capture.py capture --keys F13,F24 --out run.ndjson

python hid_capture.py keycodes            # list the bench test keycodes
```

Requirements:
- `pyobjc-framework-Quartz` (`pip install pyobjc-framework-Quartz`).
- Accessibility (or Input Monitoring) permission for the terminal or its
  responsible app, in System Settings > Privacy & Security. A listen-only
  session tap is refused without it; the tool prints that and exits 2.

Correlating a run: the injector records the host-monotonic time of each tap; the
capture records the host-monotonic time of each delivered event. A tap with no
matching event inside a window is a loss; events out of injection order are a
reorder; a delivered press with no delivered release is a stuck key. The
firmware per-hop counters (added alongside) then say *where* a lost report was
dropped.
