# OpenController firmware — CH592F

Firmware for a WCH CH592F (QingKe RISC-V4F) wireless module that adds
2.4 GHz wireless support to QMK keyboards. The CH592F sits next to the
keyboard's host MCU, speaks a binary command/status protocol over UART1,
and implements the keyboard-side wireless link used by
[OpenDongle](https://github.com/openkeyboard-org): pairing, connected-mode
channel hopping, HID report uplink, and persistent bonding in DataFlash.

Status: the 2.4 GHz connected-mode HID link is bench-validated end-to-end
against both OpenDongle and a stock production receiver (fresh pair, bonded
reconnect, HID delivery, reset recovery). BLE HID/OTA, deep sleep, and real
battery reporting are follow-up work.

## Toolchain (hard requirement)

Building requires the **MounRiver/WCH GCC12** toolchain
(`riscv-wch-elf-gcc` 12.2.0). The closed-source CH59x BLE library is
compiled with WCH's `WCH-Interrupt-fast` (HPE) interrupt semantics; with
mainline GCC or `INT_SOFT=1` the link succeeds but the image is silently
dead (no IRQs delivered). The Makefile enforces `INT_SOFT=0` and documents
the bench matrix behind this.

```bash
make MRS_TOOLCHAIN="/path/to/MounRiver/RISC-V Embedded GCC12/bin"
```

`MRS_TOOLCHAIN` defaults to `$HOME/Development/Mounriver/Toolchain/RISC-V
Embedded GCC12/bin`; override it for your install.

## Build

```bash
make            # build/opencontroller_ch592f.{elf,hex,bin} + size check
make clean
```

The size check enforces the 216 KB app budget (see flash map below).

## Flash map and flashing

The app is linked at flash offset `0x1000`, preserving a 4 KB stage-1
trampoline page at `0x0000..0x0FFF`. The linker caps the app at 216 KB
(`0x36FFF`) because the regions above it are owned by the stock-compatible
boot chain:

- `0x37000..0x6CFFF` — IAP staging region, wiped on every IAP flash;
- `0x6D000..0x6FFFF` — stage-1 code; overwriting it bricks the boot path.

Flashing uses [minichlink](https://github.com/cnlohr/ch32fun) (from
`PATH`, or `make MINICHLINK=/path/to/minichlink`) with a WCH-LinkE probe.
`KBD_PROBE=<serial>` selects a probe when several are attached.

```bash
make flash-app                  # app slot only; boot chain must be present
make flash-bare                 # bare fixture: trampoline page + app
```

For source-level debugging prefer the WCH/MounRiver fork of OpenOCD
(mainline OpenOCD lacks the `wlinke` adapter and `sdi` transport; pass
`-c "chip_id CH59x"` or the probe fails to connect). minichlink is fine
for flashing but resets the target on every access, which perturbs
post-mortem state.

## Build configuration

Validated-default knobs (part of the bench-validated RF behavior — change
only for A/B experiments):

| Knob | Default | Meaning |
|---|---|---|
| `RF_DIAG_COUNTERS` | 1 | SWD-readable `.diag_safe` diagnostic counters |
| `WATCHDOG_ENABLE` | 1 | independent WWDG last-resort recovery |
| `LINK_LOCK_SERVO_FIX` | 1 | bipolar ±1 connected-mode hop servo |
| `RF_TURNAROUND_COUNT` | 6000 | post-poll TX turnaround (~100 µs @ 60 MHz) |
| `STOCK_RESP_NO_SETCH` | 1 | skip redundant pre-response `RF_SetChannel` |
| `STOCK_CONNECT_IDX_BIAS` | 1 | first connected hop uses seed+1 |

Bench-only interop probes (`STOCK_SUPPRESS_RESPONSES`,
`STOCK_PARK_AFTER_CATCH`, `STOCK_SEED_BOND`, …) default off via `#ifndef`
fallbacks in `src/rf_task.c` and are enabled with
`make EXTRA_CFLAGS=-DSTOCK_...=1`.

The BLE library defaults to the vendored V1.4.2 triple; other versions are
drop-in directories selected with `make BLE_LIB_DIR=...`.

## Layout and licensing

- `src/`, `ld/`, `Makefile`, `docs/` — first-party, Apache-2.0
  (see `LICENSE`).
- `vendor/` — WCH SDK subset and BLE library, **not** Apache-2.0; see
  `NOTICE` and `vendor/README.md` for terms, provenance, and checksums.
- `docs/TMOS_REVIEW.md` — SDK-alignment review; the written rationale for
  interrupt-architecture decisions (e.g. why there is no app
  `BB_IRQHandler`).
