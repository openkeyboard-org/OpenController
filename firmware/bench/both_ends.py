#!/usr/bin/env python3
"""Count one run from both ends and reconcile.

The keyboard's .diag_safe counters are cumulative (NOLOAD, deliberately not
zeroed at startup), so they are read before and after and differenced. The
receiver's counters are zeroed by its reboot, so they are read the same way for
symmetry rather than assumed to start at zero.

The reconciliation is the point:
  sealed_delta  vs  enc_shape_delta   -> did every sealed frame reach the sink?
  enc_shape     vs  ok + mac          -> did every arriving frame get verified?
A shortfall in the first is a transmit/air-loss problem; a shortfall in the
second is a crypto-state problem. They point at completely different code.
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
HOLD_S = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0

# .diag_safe layout, from build/ch592f-slotA/opencontroller_ch592f.map
KBD_BASE = 0x200059DC
KBD_FIELDS = ["seal_miss", "tx_sealed", "sess_bad", "sess_ok", "sess_rx"]

t0 = time.time()


def log(m):
    print(f"[{time.time() - t0:6.2f}s] {m}", flush=True)


def read_kbd():
    out = subprocess.run(
        [MINICHLINK, "-l", KBD_PROBE, "-r", "+", hex(KBD_BASE),
         hex(4 * len(KBD_FIELDS))],
        capture_output=True, text=True, timeout=40).stdout
    blob = b""
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]{8}: ((?:[0-9a-f]{2} )+)", line)
        if m:
            blob += bytes.fromhex(m.group(1).replace(" ", ""))
    if len(blob) < 4 * len(KBD_FIELDS):
        return None
    vals = struct.unpack_from("<%dI" % len(KBD_FIELDS), blob, 0)
    return dict(zip(KBD_FIELDS, vals))


def read_rx(tries=6):
    """Retry: the dongle NAKs the IAP IN endpoint while it is busy, and a
    minichlink reset on the other end of the bench perturbs its timing."""
    for _ in range(tries):
        dev = find_hidraw(0x0C45, 0xFEFE, 4)
        if not dev:
            continue
        fd = os.open(dev, os.O_RDWR)
        r = txn(fd, 0x94)
        os.close(fd)
        if r and r[0] == 0x94 and r[1] >= 46:
            break
    else:
        return None
    if not r or r[0] != 0x94 or r[1] < 46:
        return None
    reasons = struct.unpack_from("<6I", r, 6)
    conn_rx, enc_shape, fifo_full, flush_drop, plain_drop = \
        struct.unpack_from("<5I", r, 30)
    return dict(ok=struct.unpack_from("<I", r, 2)[0], shape=reasons[1],
                inactive=reasons[2], mac=reasons[3], replay=reasons[4],
                engine=reasons[5], conn_rx=conn_rx, enc_shape=enc_shape,
                fifo_full=fifo_full, flush=flush_drop, plain=plain_drop,
                len_max=r[50], len_max_tag=r[51])


def frame(body):
    return bytes(body) + bytes([sum(body) & 0xFF])


# The SWD read resets the target, and startup zeroes .diag_safe -- so this
# call IS the baseline: it leaves the keyboard freshly booted with every
# counter at zero, and the post-run read is the run's total outright.
kbd0 = read_kbd()
log(f"keyboard before (reset by the SWD read): {kbd0}")
rx0 = read_rx()
if rx0 is None:
    log("ABORT: receiver diag unreadable")
    sys.exit(1)
log(f"receiver before: ok={rx0['ok']} enc_shape={rx0['enc_shape']} "
    f"mac={rx0['mac']} conn_rx={rx0['conn_rx']}")

ser = serial.Serial(KBD, 115200, timeout=0.02)
time.sleep(2.0)
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
if not connected:
    log("ABORT: no CONNECTED")
    sys.exit(1)
log("CONNECTED")

drops = []
end = time.time() + HOLD_S
while time.time() < end:
    u = ser.read(64)
    for i in range(len(u) - 1):
        if u[i] == 0x5B and u[i + 1] in (0x33, 0x35):
            drops.append(round(time.time() - t0, 2))
            log(f"!! DROP 5B {u[i+1]:02X}")
ser.close()

rx1 = read_rx()
kbd1 = read_kbd()
log(f"keyboard after: {kbd1}")

sealed = kbd1["tx_sealed"] - kbd0["tx_sealed"]
miss = kbd1["seal_miss"] - kbd0["seal_miss"]
sess_rx = kbd1["sess_rx"] - kbd0["sess_rx"]
sess_ok = kbd1["sess_ok"] - kbd0["sess_ok"]
sess_bad = kbd1["sess_bad"] - kbd0["sess_bad"]
arrived = rx1["enc_shape"] - rx0["enc_shape"]
ok = rx1["ok"] - rx0["ok"]
mac = rx1["mac"] - rx0["mac"]
conn = rx1["conn_rx"] - rx0["conn_rx"]

print()
print("=== keyboard (transmit) ===")
print(f"  frames sealed        {sealed}")
print(f"  seal misses          {miss}")
print(f"  session frames rx    {sess_rx}  (ok {sess_ok}, bad {sess_bad})")
print("=== receiver (receive) ===")
print(f"  connected RX         {conn}")
print(f"  largest LEN seen     {rx1['len_max']}  tag 0x{rx1['len_max_tag']:02X}")
print(f"  encrypted accepted   {arrived}")
print(f"  verified             {ok}")
print(f"  MAC failures         {mac}")
print(f"  fifo_full / flush    {rx1['fifo_full'] - rx0['fifo_full']}"
      f" / {rx1['flush'] - rx0['flush']}")
print("=== reconciliation ===")
lost = sealed - arrived
print(f"  sealed {sealed} -> arrived {arrived}   (lost in transit: {lost}"
      f"{'' if sealed == 0 else f' = {100*lost/sealed:.1f}%'})")
print(f"  arrived {arrived} -> verified {ok} + mac-failed {mac}"
      f"   (unaccounted {arrived - ok - mac})")
print(f"  link drops: {len(drops)} {drops}")
