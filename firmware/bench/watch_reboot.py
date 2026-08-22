#!/usr/bin/env python3
"""Is the keyboard rebooting when the encrypted link drops?

kbd_crypt_tx_sealed lives in .diag_safe and is zeroed only by RF_TaskInit(),
which runs once per boot. It otherwise increases monotonically. So a DECREASE
between two samples is a reboot, full stop -- no extra symbol knowledge needed.
SWD reads have been verified not to reset the target (two consecutive reads of
ll_boot_count returned identical values), so the probe is not the thing being
measured here.
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

MINICHLINK = "/home/emolitor/Development/Personal/WCH/ch32fun/minichlink/minichlink"
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
