# Rest hold (T_idle) sweep and dongle-outage ladder, 2026-09-13

Decides RESTING_POLICY.md D1: `KBD_REST_IDLE_TICKS` 2 s / 3 s / 5 s, measured on the
controller rail, with delivery and latency as the guard rails. Outcome: **3 s default**
(`make KBD_REST_IDLE_MS=3000`; production parity `=5000`).

## Bench

Nucleo STM32U083 running QMK `handwired/opencontroller_bench` (oracle keymap, F13; host
forced to BLUETOOTH so the dongle is the only path) -> controller CH592 module (this
repo, `main` 47994ff, only the hold changed via the build) -> OpenDongle CH592 (main
254135b) -> macOS. PPK2 inline on the controller rail at 100 kSps; this devboard has a
constant ~1.55 mA power LED, all figures below are net of it. Host oracle: CGEventTap
(`firmware/bench/tools/hid_capture.py`), scored as exactly one ordered down/up per key on
a continuous capture. RSSI -37 dBm throughout.

Protocol (after a Codex refutation pass): identical flags on build and flash with the
image hash logged; controller diag counters (`bench.py diag`, frame v3) snapshotted per
arm; full-tail raw capture per key (reconnect, hold, rest entry, lock); floor = p05 of the
settled window before the key, resting counterfactual subtracted; jittered rest depths;
a seeded 60-key typing workload identical across holds; the 5 s control re-measured at
the end (39.0 -> 39.3 mC/key, workload 875 -> 873 mC: no drift). Scripts: `hold_sweep2.py`;
the build/flash side (identical flags on both make invocations, image hash per arm) is
`flash_hold.sh`, and its `FLASH ...` lines are in the archived `hold_sweep.log` next to every
result line. The archived runs were scored with an earlier `score_all` whose window fallback
did not count stray events; every archived phase logged `events total` = 2 x keys, so none
occurred. The script now reports strays explicitly. The workload's reconnect-key counts
below (11 / 15 / 22) exclude key 0, which also starts from settled rest (the script now
includes it: 12 / 16 / 23).

The scripts take their bench paths from the environment (`OPENKEYBOARD_QMK`, `OPENDONGLE_TOOL`,
`MINICHLINK`, `DONGLE_PROBE`, `KBD_PROBE`, `PPK2D_SOCK`, `HID_CAPTURE`, `BENCH_PY`,
`BENCH_FLOOR_MA`); the defaults are this bench's.

## Charge per key from rest, first-key delivery, workload

| hold | mC per key from rest (n=6) | hold burst | far/near delivery (24/12) | latency med/max (far, near) | workload, 60 keys: active / to-lock | reconnect keys (excl. key 0) |
|---|---|---|---|---|---|---|
| 5 s | 39.0 (sd 0.3) | 5.08 s | 24/24, 12/12 | 70/91, 50/116 ms | 875 / 915 mC | 11 |
| 3 s | 23.8 (sd 0.8) | 3.08 s | 24/24, 12/12 | 69/90, 70/169 ms | 692 / 716 mC | 15 |
| 2 s | 16.8 (sd 0.6) | 2.13 s | 24/24, 12/12 | 70/149, 50/90 ms | 547 / 564 mC | 22 |

Model: **charge per key from rest ~ 7.4 mA x T_idle + 2 mC**; the 2 mC is reconnect and
rest entry, the rest is the CONNECTED hold. Steady resting is ~0.02-0.03 mA. Held keys
deliver in ~8 ms; reconnect keys typically in 30-90 ms at every hold, with observed maxima
of 116 / 169 / 149 ms at 5 / 3 / 2 s; post-rest lock is immediate (no churn) in 18/18
episodes. Not measured: a lossy link (all runs at -37 dBm).

## Dongle-outage ladder (`drought.py`, `drought2.py`)

The dongle's rail (`minichlink -kt/-k3` on its WCH-Link) cut for a nominal 1.5/2.5/4/8/30 s
(each rail command takes ~0.5 s, so the actual outage runs ~0.5-1 s longer; the script now
logs the measured bounds) while the controller held a live link, 1 s into the post-key hold; the dongle re-enumerates in
~1.1 s and then only camps, so the poll gap always exceeds the controller's fixed 3.125 s
supervision.

| | 5 s hold | 3 s hold |
|---|---|---|
| path on every outage | supervision -> IDLE, `5B 33`, radio off | rest (silent; driver never told) |
| next key ~15 s after the dongle returns | 0/5 (QMK driver defect, below) | 5/5, 37-95 ms |
| keys at +5..+60 s after a 2.5 s outage | 0/5 | 5/5 |
| controller while the dongle is absent | 0 mA (dead link) | ~0.7 mA probing until the driver's sleep timeout |

**QMK driver defect (fixed, emolitor/qmk_firmware `em-stm32u073` 603c49bd17):** after the
module's `5B 33` the driver parked the keyboard report (the protocol layer only sends
while CONNECTED) and nothing re-drove the bonded reconnect until its own sleep timeout;
the UART trace showed the matrix event with no TX at all. Housekeeping now treats a
pending report on a DISCONNECTED 2.4 GHz link as latched demand and re-drives the
reconnect (queue-idle guard, 500 ms backoff, module awake). After the fix the 5 s hold
recovers too: 3/3, 3/3, ladder 3/3, normal path 12/12 unchanged. A key with the dongle
absent runs the bonded search at ~1.7 mA net until `OPENCONTROLLER_SLEEP_TIMEOUT_MS`.

**Dongle defect (OpenDongle PR #52):** the first key after a dongle reset while the module
was asleep was sometimes lost with the link up: keyboard `ll_hid_tx_done_down` advanced
and the ack-retired FIFO released the head, dongle `rfd_hid_rx` unchanged. A race (6/6 on
one image, 0/10 on another); the dongle applied its control-byte feedback to every good
RX before its state switch, so a frame it never admitted was still acked. Made
deterministic with a bench knob and fixed by gating the feedback on admission.

## Traps found

- `bench.c` scheduled tap: re-arming while a press is in flight cancels its release
  (STUCK then a no-op press); keep arms >= delay + duration apart.
- `flash-factory` needs the module awake through make's startup and the bond read; with a
  short hold the link rests (deep sleep, SWD off) first -- hold it with a tap loop.
- The KBD2 bond guard's `grep "4b 42 44 32"` never matched BSD `od`'s double-space
  padding (fixed in this change).
- Every SWD read of the CH592 dongle resets it (the controller's do not).
