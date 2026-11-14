#!/usr/bin/env python3
# Copyright 2026 Eric Molitor (@emolitor)
# SPDX-License-Identifier: Apache-2.0
"""Per-report keystroke oracle for the report-delivery bench.

The macOS HID idle timer used earlier only tells you *some* input arrived; it
cannot identify the key, distinguish a press from a release, or preserve order.
This tool installs a listen-only CGEventTap and logs every keyDown / keyUp /
modifier change for a chosen set of keys, with a millisecond timestamp, so a
sequence of injected reports can be checked for delivery AND order AND that
every release arrived.

Attribution is by key choice, exactly as the bench already does: inject a key
the human is not pressing (F13/F24, or a bare modifier), and every event of
that key during the window came from the device under test.

Requires: pyobjc-framework-Quartz, and the terminal/process must have
Accessibility (or Input Monitoring) permission -- a listen-only session tap is
refused otherwise, and this tool says so and exits 2.

Usage:
  hid_capture.py capture [--keys 105,111] [--out FILE] [--seconds N] [--quiet]
  hid_capture.py selftest            # inject a synthetic F13 and confirm the tap sees it
  hid_capture.py keycodes            # print the bench test keycodes

Output is NDJSON, one event per line, to stdout (line-buffered) and optionally
to --out. Fields: {"t_ms": float, "ev": "down"|"up"|"flags", "keycode": int,
"flags": int}. t_ms is a monotonic host clock, comparable to the injector's.
"""
import argparse
import json
import signal
import sys
import time

import Quartz
from Quartz import (
    CGEventTapCreate,
    CGEventTapEnable,
    CGEventGetIntegerValueField,
    CGEventGetFlags,
    CFMachPortCreateRunLoopSource,
    CFRunLoopAddSource,
    CFRunLoopGetCurrent,
    CFRunLoopRun,
    CFRunLoopStop,
    kCGHeadInsertEventTap,
    kCGEventTapOptionListenOnly,
    kCGEventKeyDown,
    kCGEventKeyUp,
    kCGEventFlagsChanged,
    kCGKeyboardEventKeycode,
    kCGEventTapDisabledByTimeout,
    kCGEventTapDisabledByUserInput,
)

# CGEventTapLocation enum -- not exported by name in every pyobjc build.
kCGHIDEventTapLocation = 0
kCGSessionEventTapLocation = 1

# macOS virtual keycodes the bench uses as device-attributable test keys.
# macOS defines virtual keycodes only up to F20 (Carbon HIToolbox Events.h); there
# is no kVK_F24, and 111 is kVK_F12 -- listing it as "F24" was wrong and made an
# F24 capture look merely empty rather than impossible. A test key is only usable
# here if macOS surfaces a CGEvent for it, so F24 (HID 0x73) cannot be one.
KEYCODES = {
    "F13": 105, "F14": 107, "F15": 113, "F16": 106, "F17": 64,
    "F18": 79, "F19": 80, "F20": 90, "F12": 111,
    "A": 0,  # only if nothing else is typing 'a'
    "LCTRL": 59, "LSHIFT": 56, "LALT": 58, "LGUI": 55,
}
_EVMASK = ((1 << kCGEventKeyDown) | (1 << kCGEventKeyUp) | (1 << kCGEventFlagsChanged))


def _mono_ms():
    return time.monotonic() * 1000.0


class Capture:
    def __init__(self, keys, out=None, quiet=False):
        self.keys = set(keys) if keys else None  # None = all keys
        self.out = open(out, "w") if out else None
        self.quiet = quiet
        self.n = 0
        self.tap = None

    def emit(self, ev, keycode, flags):
        if self.keys is not None and keycode not in self.keys:
            return
        rec = {"t_ms": round(_mono_ms(), 3), "ev": ev, "keycode": int(keycode), "flags": int(flags)}
        line = json.dumps(rec)
        if not self.quiet:
            print(line, flush=True)
        if self.out:
            self.out.write(line + "\n")
            self.out.flush()
        self.n += 1


def _callback(proxy, etype, event, cap):
    try:
        if etype in (kCGEventTapDisabledByTimeout, kCGEventTapDisabledByUserInput):
            # macOS disables a listen tap across system sleep / long stalls; re-arm it.
            tap = getattr(cap, "tap", None)
            if tap is not None:
                CGEventTapEnable(tap, True)
            return event
        if etype == kCGEventFlagsChanged:
            kc = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode)
            cap.emit("flags", kc, CGEventGetFlags(event))
        else:
            kc = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode)
            cap.emit("down" if etype == kCGEventKeyDown else "up", kc, CGEventGetFlags(event))
    except Exception as e:  # a tap callback must never raise into CoreFoundation
        sys.stderr.write(f"callback error: {e!r}\n")
    return event  # listen-only: pass the event through unchanged


def _make_tap(cap):
    tap = CGEventTapCreate(
        kCGSessionEventTapLocation, kCGHeadInsertEventTap,
        kCGEventTapOptionListenOnly, _EVMASK, _callback, cap,
    )
    if tap is None:
        sys.stderr.write(
            "CGEventTapCreate returned None: this process lacks Accessibility / Input\n"
            "Monitoring permission. Grant it to the terminal (or the responsible app) in\n"
            "System Settings > Privacy & Security > Accessibility, then re-run.\n")
        sys.exit(2)
    src = CFMachPortCreateRunLoopSource(None, tap, 0)
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, Quartz.kCFRunLoopCommonModes)
    CGEventTapEnable(tap, True)
    cap.tap = tap   # so the callback can re-enable after a sleep/timeout disable
    return tap


def cmd_capture(a):
    cap = Capture([_resolve(k) for k in a.keys.split(",")] if a.keys else None, a.out, a.quiet)
    _make_tap(cap)
    loop = CFRunLoopGetCurrent()
    timer = None
    if a.seconds:
        # stop the run loop after N seconds via a background timer thread; daemonised
        # so a Ctrl-C exit is never held open by a pending timeout
        import threading
        timer = threading.Timer(a.seconds, lambda: CFRunLoopStop(loop))
        timer.daemon = True
        timer.start()
    signal.signal(signal.SIGINT, lambda *_: CFRunLoopStop(loop))
    sys.stderr.write(f"capturing {'all keys' if cap.keys is None else sorted(cap.keys)}"
                     f"{' for %.0fs' % a.seconds if a.seconds else ' (Ctrl-C to stop)'}\n")
    CFRunLoopRun()
    if timer is not None:
        timer.cancel()
    if cap.out:
        cap.out.close()
    sys.stderr.write(f"captured {cap.n} events\n")


def _resolve(k):
    k = k.strip()
    if k.upper() in KEYCODES:
        return KEYCODES[k.upper()]
    return int(k, 0)


def cmd_selftest(a):
    """Post a synthetic F13 down/up and confirm the tap observes it. Validates
    the tap plumbing without any hardware; a real device is not involved."""
    from Quartz import CGEventCreateKeyboardEvent, CGEventPost
    cap = Capture([KEYCODES["F13"]], quiet=True)
    _make_tap(cap)
    loop = CFRunLoopGetCurrent()
    seen = []
    orig = cap.emit
    def spy(ev, kc, fl):
        seen.append((ev, kc)); orig(ev, kc, fl)
        if len(seen) >= 2:
            CFRunLoopStop(loop)
    cap.emit = spy
    import threading
    def inject():
        time.sleep(0.3)
        for down in (True, False):
            CGEventPost(kCGHIDEventTapLocation,
                        CGEventCreateKeyboardEvent(None, KEYCODES["F13"], down))
            time.sleep(0.02)
    threading.Thread(target=inject, daemon=True).start()
    guard = threading.Timer(3.0, lambda: CFRunLoopStop(loop))
    guard.daemon = True
    guard.start()
    CFRunLoopRun()
    guard.cancel()
    ok = [("down", 105), ("up", 105)] == seen[:2]
    print("selftest:", "PASS" if ok else f"FAIL (saw {seen})")
    sys.exit(0 if ok else 1)


def cmd_keycodes(a):
    for name, code in sorted(KEYCODES.items(), key=lambda x: x[1]):
        print(f"{code:3d}  {name}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture"); c.add_argument("--keys", default="F13,F24")
    c.add_argument("--out"); c.add_argument("--seconds", type=float, default=0.0)
    c.add_argument("--quiet", action="store_true"); c.set_defaults(fn=cmd_capture)
    sub.add_parser("selftest").set_defaults(fn=cmd_selftest)
    sub.add_parser("keycodes").set_defaults(fn=cmd_keycodes)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
