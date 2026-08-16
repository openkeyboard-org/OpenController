#!/usr/bin/env python3
"""Read the receiver's bench UART telemetry (OpenDongle uart_diag.c, 0x5E frames).

The USB-less bench replacement for crypt_diag.py: the receiver broadcasts one
127-byte frame per second on UART1 PA9 -> WCH-Link CF148F065446 -> ttyACM.

Usage:
    rx_uart_diag.py [--port PATH] [--seconds N] [--follow]

One-shot (default): print the first valid frame and exit.
--follow: keep printing frames (one line per frame) until interrupted; a final
full decode of the last frame is printed on exit. Deltas are against the first
valid frame seen.
"""
import argparse
import struct
import sys
import time

import serial

PORT = "/dev/serial/by-id/usb-wch.cn_WCH-Link_CF148F065446-if01"
SOF, PAYLOAD_LEN = 0x5E, 124
FRAME_LEN = PAYLOAD_LEN + 3

REASONS = ["OK", "DROP_SHAPE", "DROP_INACTIVE", "DROP_MAC", "DROP_REPLAY",
           "FAULT_ENGINE"]


def parse(payload: bytes) -> dict:
    d = {}
    d["ok"] = struct.unpack_from("<I", payload, 0)[0]
    reasons = struct.unpack_from("<6I", payload, 4)
    for i, name in enumerate(REASONS):
        if i:
            d[name.lower()] = reasons[i]
    (d["conn_rx"], d["enc_shape"], d["fifo_full"], d["flush_drop"],
     d["plain_drop"]) = struct.unpack_from("<5I", payload, 28)
    d["len_max"], d["len_max_tag"] = payload[48], payload[49]
    (d["mint"], d["mac_same_ok"], d["last_mac_ctr"], d["mac_prev_ok"],
     d["same_differs"], d["bb_during_aes"]) = struct.unpack_from("<6I", payload, 50)
    d["kat_run"], d["kat_fail"] = payload[74], payload[75]
    d["fail_latched"], d["fail_len"] = payload[76], payload[77]
    d["fail_session"], d["fail_counter"] = struct.unpack_from("<II", payload, 78)
    d["fail_expect1"] = payload[86:94].hex()
    d["fail_expect2"] = payload[94:102].hex()
    d["fail_frame"] = payload[102:102 + 22].hex()
    return d


def frames(ser, deadline):
    buf = b""
    while time.time() < deadline:
        buf += ser.read(256)
        while True:
            i = buf.find(bytes([SOF]))
            if i < 0:
                buf = b""
                break
            if len(buf) - i < FRAME_LEN:
                buf = buf[i:]
                break
            cand = buf[i:i + FRAME_LEN]
            if cand[1] == PAYLOAD_LEN and \
               (sum(cand[:FRAME_LEN - 1]) & 0xFF) == cand[FRAME_LEN - 1]:
                buf = buf[i + FRAME_LEN:]
                yield parse(cand[2:2 + PAYLOAD_LEN])
            else:
                buf = buf[i + 1:]          # false SOF, resync


def show(d):
    print(f"  verified (ok)      {d['ok']}")
    for name in REASONS[1:]:
        print(f"  drop {name:<14} {d[name.lower()]}")
    print(f"  conn_rx {d['conn_rx']}  enc_shape {d['enc_shape']}  "
          f"len_max {d['len_max']}/0x{d['len_max_tag']:02X}  "
          f"fifo_full {d['fifo_full']}  flush {d['flush_drop']}  "
          f"plain {d['plain_drop']}")
    print(f"  mint {d['mint']}  MAC_SAME_OK {d['mac_same_ok']}  "
          f"same_differs {d['same_differs']}  bb_during_aes {d['bb_during_aes']}")
    print(f"  kat run/fail {d['kat_run']}/{d['kat_fail']}  "
          f"last_mac_ctr {d['last_mac_ctr']}")
    if d["fail_latched"]:
        print(f"  LATCH: len {d['fail_len']} session {d['fail_session']:08X} "
              f"ctr {d['fail_counter']}")
        print(f"    frame   {d['fail_frame'][:2 * d['fail_len']]}")
        print(f"    expect1 {d['fail_expect1']}  expect2 {d['fail_expect2']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=PORT)
    ap.add_argument("--seconds", type=float, default=5.0,
                    help="listen window (one-shot) / total follow time")
    ap.add_argument("--follow", action="store_true")
    a = ap.parse_args()

    ser = serial.Serial(a.port, 115200, timeout=0.2)
    deadline = time.time() + a.seconds
    base, last, n = None, None, 0
    try:
        for d in frames(ser, deadline):
            n += 1
            if base is None:
                base = d
            last = d
            if not a.follow:
                show(d)
                return 0
            dm = d["drop_mac"] - base["drop_mac"]
            ok = d["ok"] - base["ok"]
            print(f"[{time.strftime('%H:%M:%S')}] ok +{ok} mac +{dm} "
                  f"same_ok {d['mac_same_ok']} differs {d['same_differs']} "
                  f"bb {d['bb_during_aes']} mint {d['mint']} "
                  f"latch {d['fail_latched']}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    if last is None:
        print("no valid 0x5E frame seen — receiver silent or wrong port")
        return 1
    if a.follow:
        print(f"\n=== last frame (of {n}) ===")
        show(last)
        if base is not None and last is not base:
            print(f"=== deltas over run ===")
            print(f"  ok +{last['ok'] - base['ok']}  "
                  f"drop_mac +{last['drop_mac'] - base['drop_mac']}  "
                  f"mac_same_ok +{last['mac_same_ok'] - base['mac_same_ok']}  "
                  f"same_differs +{last['same_differs'] - base['same_differs']}  "
                  f"mint +{last['mint'] - base['mint']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
