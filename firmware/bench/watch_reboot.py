#!/usr/bin/env python3
"""Is the keyboard rebooting when the encrypted link drops? -- OBSOLETE, OPT-IN.

kbd_crypt_tx_sealed lives in .diag_safe and is zeroed only by RF_TaskInit(),
which runs once per boot. It otherwise increases monotonically, so a DECREASE
between two samples is a reboot. That part still holds.

**The measurement method does not.** This script polls the counter over SWD with
`minichlink -r`, and the docstring used to claim "SWD reads have been verified
not to reset the target (two consecutive reads of ll_boot_count returned
identical values)". That is not evidence: two equal reads are exactly what a
probe that resets the target and re-reads a freshly zeroed counter would also
produce. It contradicts this repo's own docs/TODO.md section 6 and
bench/README.md, both of which say probe attach resets the target and CH5xx SWD
reads are unreliable -- so this tool can manufacture the very reboots it
reports. Measured 2026-08-23 on the OpenDongle bench, minichlink cannot connect
to a CH5xx part at all (rc=223), so `read_kbd()` here simply fails.

Use the UART `0xAF` crypt-diag telemetry instead (`rx_uart_diag.py`,
`bench_run.py`), which reads the same counters without touching the debug port.

Kept only for reference. It refuses to run unless you set
WATCH_REBOOT_I_KNOW_SWD_RESETS=1, so nobody re-derives a reboot rate from it by
accident.
"""
import os
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crypt_diag import find_hidraw, txn  # noqa: E402
import serial  # noqa: E402

if os.environ.get("WATCH_REBOOT_I_KNOW_SWD_RESETS") != "1":
    sys.exit(
        "watch_reboot.py is obsolete: it polls the keyboard over SWD, and probe\n"
        "attach resets the target (docs/TODO.md section 6, bench/README.md), so\n"
        "it can manufacture the reboots it reports. minichlink also cannot reach\n"
        "a CH5xx part at all on this bench.\n"
        "Use the UART 0xAF telemetry instead: rx_uart_diag.py / bench_run.py.\n"
        "To run it anyway: WATCH_REBOOT_I_KNOW_SWD_RESETS=1 watch_reboot.py")

MINICHLINK = os.environ.get(
    "MINICHLINK",
    os.path.expanduser("~/Development/Personal/WCH/ch32fun/minichlink/minichlink"))
KBD_PROBE = "CEBD8F0653EF"
KBD = "/dev/serial/by-id/usb-wch.cn_WCH-Link_CEBD8F0653EF-if01"
KBD_BASE = 0x200059DC
FIELDS = ["seal_miss", "tx_sealed", "sess_bad", "sess_ok", "sess_rx"]
HOLD_S = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0

t0 = time.time()


def log(m):
    print(f"[{time.time() - t0:6.2f}s] {m}", flush=True)


def read_kbd():
    out = subprocess.run(
        [MINICHLINK, "-l", KBD_PROBE, "-r", "+", hex(KBD_BASE), "0x14"],
        capture_output=True, text=True, timeout=30).stdout
    blob = b""
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]{8}: ((?:[0-9a-f]{2} )+)", line)
        if m:
            blob += bytes.fromhex(m.group(1).replace(" ", ""))
    if len(blob) < 20:
        return None
    return dict(zip(FIELDS, struct.unpack_from("<5I", blob, 0)))


def read_rx():
    dev = find_hidraw(0x0C45, 0xFEFE, 4)
    if not dev:
        return None
    fd = os.open(dev, os.O_RDWR)
    r = txn(fd, 0x94, timeout=0.4)
    os.close(fd)
    if not r or r[0] != 0x94 or r[1] < 46:
        return None
    reasons = struct.unpack_from("<6I", r, 6)
    conn_rx, enc_shape, fifo_full, flush, plain = struct.unpack_from("<5I", r, 30)
    return dict(ok=struct.unpack_from("<I", r, 2)[0], mac=reasons[3],
                replay=reasons[4], conn_rx=conn_rx, enc_shape=enc_shape)


def frame(body):
    return bytes(body) + bytes([sum(body) & 0xFF])


ser = serial.Serial(KBD, 115200, timeout=0.02)
time.sleep(1.5)
ser.read(4096)
ser.write(frame([0xA6, 0x30]))
log("keyboard -> 2.4G")

connected = False
end = time.time() + 25.0
while time.time() < end and not connected:
    d = ser.read(256)
    for i in range(len(d) - 1):
        if d[i] == 0x5B and d[i + 1] == 0x32:
            connected = True
log("CONNECTED" if connected else "no CONNECTED marker; continuing anyway")

prev_k, prev_r = read_kbd(), read_rx()
log(f"kbd {prev_k}")
log(f"rx  {prev_r}")

reboots, drops = [], []
end = time.time() + HOLD_S
while time.time() < end:
    u = ser.read(64)
    for i in range(len(u) - 1):
        if u[i] == 0x5B and u[i + 1] in (0x33, 0x35):
            drops.append(round(time.time() - t0, 2))
            log(f"!! DROP 5B {u[i+1]:02X}")

    k, r = read_kbd(), read_rx()
    if k and prev_k:
        if k["tx_sealed"] < prev_k["tx_sealed"]:
            t = round(time.time() - t0, 2)
            reboots.append(t)
            log(f"** KEYBOARD REBOOT: tx_sealed {prev_k['tx_sealed']} -> "
                f"{k['tx_sealed']}")
        elif k != prev_k:
            log(f"kbd {k}")
    if r and prev_r and r != prev_r:
        d = {kk: r[kk] - prev_r[kk] for kk in r if r[kk] != prev_r[kk]}
        log(f"rx  +{d}")
    prev_k, prev_r = k or prev_k, r or prev_r

ser.close()
print()
print(f"keyboard reboots: {len(reboots)} {reboots}")
print(f"link drops:       {len(drops)} {drops}")
