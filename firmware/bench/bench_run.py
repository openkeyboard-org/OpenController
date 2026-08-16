#!/usr/bin/env python3
"""One encrypted-link bench run over UART only (no dongle USB).

Sequence:
  1. open BOTH probe CDC ports once (each open DTR-resets its target; both
     handles are then HELD -- reopening mid-run would reset a target);
  2. keyboard: A6 30 (2.4G), then A6 51 (pair) if it reports no bond;
  3. wait CONNECTED (5B 32);
  4. key the keyboard: 0xAE + BENCH_KEY -> expect 5B 21;
  5. power-cycle the RECEIVER via its probe: its bond loads and
     DONGLE_CRYPT_BENCH_FORCE_KEY activates encryption; immediately re-send
     A6 30 so the keyboard broadcasts inside the receiver's ~3 s boot window;
  6. hold HOLD_S seconds with a LINK-KEEPER: if receiver conn_rx stalls for
     >6 s, nudge (A6 30), then escalate to a receiver power cycle;
  7. read the keyboard self-verify counters (0xAF, 8 x u32) and the failure
     latch (0xB0), plus the receiver 0x5E telemetry; print the reconciliation.

Keyboard 0xAF reply: [0x5D][8 x u32 LE][chk] =
  seal_miss, tx_sealed, sess_bad, sess_ok, sess_rx,
  selfck_ok, selfck_bad, bb_during_aes
Keyboard 0xB0 reply: [0x5F][latched][len][session u32][seal_bb][frame 22][good 8][chk]

Usage: bench_run.py [hold_seconds] [--skip-pair] [--no-power-cycle]
"""
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, "/home/emolitor/Development/openkeyboard/OpenController/firmware/bench")
from rx_uart_diag import FRAME_LEN, PAYLOAD_LEN, SOF, parse, show  # noqa: E402
import serial  # noqa: E402

KBD_PORT = "/dev/serial/by-id/usb-wch.cn_WCH-Link_CEBD8F0653EF-if01"
RX_PORT = "/dev/serial/by-id/usb-wch.cn_WCH-Link_CF148F065446-if01"
MINICHLINK = "/home/emolitor/Development/Personal/WCH/ch32fun/minichlink/minichlink"
RX_PROBE = "CF148F065446"
BENCH_KEY = bytes.fromhex("4f70656e4b626421a55ac33c69960ff0")

KBD_DIAG_N = 9
KBD_DIAG_LEN = 1 + 4 * KBD_DIAG_N + 1          # 34
KBD_FAIL_LEN = 8 + 22 + 8 + 1                  # 39
DIAG_NAMES = ["seal_miss", "tx_sealed", "sess_bad", "sess_ok", "sess_rx",
              "selfck_ok", "selfck_bad", "bb_during_aes", "aes_stale"]

t0 = time.time()


def log(m):
    print(f"[{time.time() - t0:7.2f}s] {m}", flush=True)


def frame(body):
    b = bytes(body)
    return b + bytes([sum(b) & 0xFF])


class KbdEvents:
    """Scan the keyboard byte stream for 5B status, 5D diag, 5F fail-latch."""

    def __init__(self):
        self.buf = b""
        self.status = []          # (t, code)
        self.diag = None          # tuple of KBD_DIAG_N u32
        self.fail = None          # dict

    def feed(self, data):
        if data:
            self.buf += data
        changed = True
        while changed:
            changed = False
            for lead, need in ((0x5D, KBD_DIAG_LEN), (0x5F, KBD_FAIL_LEN),
                               (0x5B, 3)):
                i = self.buf.find(bytes([lead]))
                if i < 0 or len(self.buf) - i < need:
                    continue
                pkt = self.buf[i:i + need]
                if (sum(pkt[:-1]) & 0xFF) != pkt[-1]:
                    continue
                if lead == 0x5D:
                    self.diag = struct.unpack_from(f"<{KBD_DIAG_N}I", pkt, 1)
                elif lead == 0x5F:
                    self.fail = {
                        "latched": pkt[1], "len": pkt[2],
                        "session": struct.unpack_from("<I", pkt, 3)[0],
                        "seal_bb": pkt[7],
                        "frame": pkt[8:30].hex(),
                        "good": pkt[30:38].hex(),
                    }
                else:
                    self.status.append((time.time() - t0, pkt[1]))
                self.buf = self.buf[:i] + self.buf[i + need:]
                changed = True
        if len(self.buf) > 128:
            self.buf = self.buf[-128:]

    def saw(self, code, since=0.0):
        return any(c == code and t >= since for t, c in self.status)


def kbd_query(ser, ev, cmd, attr, timeout=3.0):
    setattr(ev, attr, None)
    ser.write(frame([cmd]))
    end = time.time() + timeout
    while time.time() < end and getattr(ev, attr) is None:
        ev.feed(ser.read(64))
    return getattr(ev, attr)


class RxCollector(threading.Thread):
    def __init__(self, ser):
        super().__init__(daemon=True)
        self.ser = ser
        self.frames = []
        self.stop = False

    def run(self):
        buf = b""
        while not self.stop:
            buf += self.ser.read(256)
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
                    self.frames.append((time.time() - t0,
                                        parse(cand[2:2 + PAYLOAD_LEN])))
                else:
                    buf = buf[i + 1:]


def rx_power_cycle():
    for args in (["-kt"], ["-k3"]):
        subprocess.run([MINICHLINK, "-C", "linke"] + args + ["-l", RX_PROBE],
                       capture_output=True, timeout=30)
        time.sleep(0.8)


FF4K = "/tmp/claude-1000/-home-emolitor-Development-openkeyboard-OpenDongle/ef19b556-a2a8-46c1-ad35-90786d7d221f/scratchpad/ff4k.bin"


def rx_wipe_bond():
    subprocess.run([MINICHLINK, "-C", "linke", "-a", "-l", RX_PROBE],
                   capture_output=True, timeout=30)
    subprocess.run([MINICHLINK, "-C", "linke", "-w", FF4K, "0x75000",
                    "-l", RX_PROBE], capture_output=True, timeout=60)
    rx_power_cycle()


def main():
    hold = float(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else 120.0
    skip_pair = "--skip-pair" in sys.argv
    no_cycle = "--no-power-cycle" in sys.argv
    fresh = "--fresh" in sys.argv
    verify_off = "--verify-off" in sys.argv

    kbd = serial.Serial(KBD_PORT, 115200, timeout=0.05)
    rxs = serial.Serial(RX_PORT, 115200, timeout=0.05)
    log("ports open (both targets DTR-reset; settling through OpenBoot's 10 s window)")
    time.sleep(11.0)
    kbd.read(4096)
    rxs.read(4096)

    ev = KbdEvents()
    col = RxCollector(rxs)
    col.start()

    if fresh:
        log("FRESH: wiping receiver bond via probe + clearing keyboard bond")
        rx_wipe_bond()
        kbd.write(frame([0xA6, 0x30])); time.sleep(0.5); ev.feed(kbd.read(128))
        kbd.write(frame([0xA6, 0x52])); time.sleep(0.8); ev.feed(kbd.read(128))

    kbd.write(frame([0xA6, 0x30]))
    time.sleep(0.8)
    ev.feed(kbd.read(256))
    has_bond = ev.saw(0x35) and not fresh
    log(f"A6 30 sent; keyboard bond: {'yes (5B 35)' if has_bond else 'no/unknown'}")

    if not skip_pair and not has_bond:
        kbd.write(frame([0xA6, 0x51]))
        log("A6 51: keyboard broadcasting for pair")

    conn_deadline = time.time() + 45.0
    cycled = False
    while time.time() < conn_deadline and not ev.saw(0x32):
        ev.feed(kbd.read(128))
        if not cycled and time.time() > conn_deadline - 30.0:
            log("no CONNECT yet -- power-cycling receiver mid-broadcast")
            rx_power_cycle()
            # restart the RIGHT keyboard mode inside the receiver's window:
            # a fresh pair needs the pair broadcast, a bonded one reconnects
            kbd.write(frame([0xA6, 0x51]) if (not skip_pair and not has_bond)
                      else frame([0xA6, 0x30]))
            cycled = True
    if not ev.saw(0x32):
        log("ABORT: no CONNECTED (5B 32) within 45 s")
        col.stop = True
        return 1
    log("CONNECTED")
    time.sleep(1.0)

    n_status = len(ev.status)
    kbd.write(frame(bytes([0xAE]) + BENCH_KEY))
    end = time.time() + 3.0
    keyed = None
    while time.time() < end and keyed is None:
        ev.feed(kbd.read(128))
        for t, c in ev.status[n_status:]:
            if c == 0x21:
                keyed = True
            elif c == 0x36:
                keyed = False
    if keyed is not True:
        log("ABORT: key write not accepted")
        col.stop = True
        return 1
    log("keyboard keyed (5B 21)")

    if verify_off:
        kbd.write(frame([0xB1, 0x00]))
        time.sleep(0.3)
        ev.feed(kbd.read(64))
        log("pre-seal self-verify DISABLED (B1 00) -- no canary in play")

    if not no_cycle:
        got = False
        for attempt in range(3):
            t_cycle = time.time() - t0
            log(f"receiver power cycle (bond load + force-key), attempt {attempt + 1}...")
            rx_power_cycle()
            kbd.write(frame([0xA6, 0x30]))
            re_deadline = time.time() + 10.0
            while time.time() < re_deadline and not ev.saw(0x32, since=t_cycle):
                ev.feed(kbd.read(128))
            if ev.saw(0x32, since=t_cycle):
                got = True
                break
        if not got:
            log("ABORT: no re-CONNECT after receiver power cycles")
            col.stop = True
            return 1
        log("re-CONNECTED (encrypted epoch)")
    time.sleep(1.0)

    d0 = kbd_query(kbd, ev, 0xAF, "diag")
    log(f"kbd baseline: {dict(zip(DIAG_NAMES, d0)) if d0 else None}")
    r0 = col.frames[-1][1] if col.frames else None
    if r0:
        log(f"rx baseline: ok {r0['ok']} mac {r0['drop_mac']} "
            f"same_ok {r0['mac_same_ok']} mint {r0['mint']}")

    log(f"holding {hold:.0f}s (idle) with link-keeper...")
    end = time.time() + hold
    last_note = 0.0
    last_rx_activity = time.time()
    last_conn_rx = r0["conn_rx"] if r0 else 0
    nudges = 0
    while time.time() < end:
        ev.feed(kbd.read(128))
        now_w = time.time()
        if col.frames:
            d = col.frames[-1][1]
            if d["conn_rx"] != last_conn_rx:
                last_conn_rx = d["conn_rx"]
                last_rx_activity = now_w
                nudges = 0
        if now_w - last_rx_activity > 6.0:
            if nudges < 2:
                log("link stalled -- nudge (A6 30)")
                kbd.write(frame([0xA6, 0x30]))
            else:
                log("link stalled -- receiver power cycle + A6 30")
                rx_power_cycle()
                kbd.write(frame([0xA6, 0x30]))
            nudges += 1
            last_rx_activity = now_w   # give the nudge time to act
        now = time.time() - t0
        if now - last_note >= 10.0 and col.frames:
            d = col.frames[-1][1]
            log(f"  rx: ok {d['ok']} mac {d['drop_mac']} same_ok "
                f"{d['mac_same_ok']} differs {d['same_differs']} "
                f"bb {d['bb_during_aes']} mint {d['mint']} "
                f"latch {d['fail_latched']}")
            last_note = now
    d1 = kbd_query(kbd, ev, 0xAF, "diag")
    fail = kbd_query(kbd, ev, 0xB0, "fail")
    time.sleep(1.5)
    col.stop = True
    kbd.close()
    rxs.close()

    drops = [(round(t, 1), hex(c)) for t, c in ev.status if c in (0x33, 0x35)]
    print("\n===== RUN REPORT =====")
    print(f"kbd start: {dict(zip(DIAG_NAMES, d0)) if d0 else None}")
    print(f"kbd end:   {dict(zip(DIAG_NAMES, d1)) if d1 else None}")
    if d0 and d1:
        dd = {k: b - a for k, a, b in zip(DIAG_NAMES, d0, d1)}
        print(f"kbd deltas: {dd}")
        tot = dd["selfck_ok"] + dd["selfck_bad"]
        if tot:
            print(f"  SELF-VERIFY: {dd['selfck_bad']}/{tot} bad "
                  f"({100.0 * dd['selfck_bad'] / tot:.1f}%)  "
                  f"bb_during_aes +{dd['bb_during_aes']}")
    print(f"link drops seen: {len(drops)}")
    if fail and fail["latched"]:
        print(f"\nKBD SELF-VERIFY LATCH: session {fail['session']:08X} "
              f"seal_bb {fail['seal_bb']} len {fail['len']}")
        print(f"  frame   {fail['frame'][:2 * fail['len']]}")
        print(f"  good    {fail['good']}   (recomputed on the keyboard)")
    elif fail:
        print("\nkbd self-verify latch: EMPTY (no bad seal observed on-keyboard)")
    if col.frames:
        rN = col.frames[-1][1]
        print("\n=== receiver final frame ===")
        show(rN)
        if r0:
            mac = rN["drop_mac"] - r0["drop_mac"]
            ok = rN["ok"] - r0["ok"]
            print(f"\n=== hold-window deltas ===")
            print(f"  verified +{ok}  DROP_MAC +{mac}  "
                  f"rate {100.0 * mac / max(1, ok + mac):.1f}%")
            print(f"  mac_same_ok +{rN['mac_same_ok'] - r0['mac_same_ok']}  "
                  f"same_differs +{rN['same_differs'] - r0['same_differs']}  "
                  f"mint +{rN['mint'] - r0['mint']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
