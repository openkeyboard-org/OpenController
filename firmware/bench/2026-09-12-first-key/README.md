# First key from rest: raw trial logs, 2026-09-12

Supporting records for the "First key from rest is lost" section of `RESTING_POLICY.md`.

Provenance: OpenController main `8a3e0c2` on the controller; OpenDongle main `23d8509`; the
keyboard MCU stand-in was a NUCLEO-U083RC built from `MonacoKeys-qmk_firmware` `99582b6bc2` with an
uncommitted bench patch that armed the module's sleep with `A6 56`/`A6 57` on every connect and sent
no wake preamble (`OPENCONTROLLER_MODULE_SLEEP=1`), or explicitly disarmed it (`=0`). Taps were
injected over SWD (`bench_cmd 3`, key F13) after the host had been idle for more than 12 s.

Oracle: the macOS HID idle timer (`ioreg -c IOHIDSystem`, `HIDIdleTime`), sampled just before and
just after each tap. A drop marks a host input event; it does not identify the key, prove ordering,
or show that a release arrived. `retries` is the driver's UART retransmission counter (one per wake
is the module's lost-first-byte signature); `acktmo` its aborted-transaction counter.

| file | module sleep | key hold | result |
|---|---|---|---|
| `sleep-off-hold250.log` | disarmed | 250 ms | 7/10 |
| `sleep-on-hold250.log` | armed | 250 ms | 1/10 |
| `sleep-on-hold1500.log` | armed | 1500 ms | 5/10 |
| `sleep-off-with-dongle-counters.log` | disarmed | 250 ms | 3/10; per trial, the dongle's `connected_promotes` and `link_rx` deltas across the tap |

The bench harness that drove these (a host build of the stand-in's driver over the probe UART, and
the SWD tap scripts) is not part of this repository.
