# OpenController firmware — CH592F

Firmware for a WCH CH592F (QingKe RISC-V4F) wireless module that adds
2.4 GHz wireless support to QMK keyboards. The CH592F sits next to the
keyboard's host MCU, speaks a binary command/status protocol over UART1,
and implements the keyboard-side wireless link used by
[OpenDongle](https://github.com/openkeyboard-org/OpenDongle): pairing, connected-mode
channel hopping, HID report uplink, and persistent bonding in DataFlash.

Status: the 2.4 GHz connected-mode HID link is bench-validated end-to-end
against both OpenDongle and a stock production receiver (fresh pair, bonded
reconnect, HID delivery, reset recovery). BLE HID/OTA, deep sleep, and real
battery reporting are follow-up work.

> **Bench note — testing reset recovery:** reset the keyboard physically
> (power cycle or the board's reset button), never through the debug probe.
> Probe-mediated resets — minichlink SDI resets/halts, or the CDC DTR reset
> that fires whenever the host opens the probe's serial port — perturb the
> chip and can show a spurious recovery failure (receiver left half-open,
> keyboard never rejoins) plus a TMOS misaligned-load fault under halt/read.
> This is a measurement artifact, not a firmware defect: a 1000-cycle
> randomized warm-reset soak with a clean USB-HID oracle recovered 999/1000
> (~2.3 s reconnect). See `BOOT.md` (Known limits) for the full history.

## Toolchain (a WCH fork of GCC is a hard requirement)

Building requires a **MounRiver/WCH** toolchain. Two are supported:

- **WCH GCC15** (`riscv32-wch-elf-gcc` 15.2.0) — the default.
- **WCH GCC12** (`riscv-wch-elf-gcc` 12.2.0) — supported alternate:

  ```bash
  make MRS_TOOLCHAIN="/path/to/MounRiver/RISC-V Embedded GCC12/bin"
  ```

`MRS_TOOLCHAIN` defaults to `$HOME/Development/Mounriver/Toolchain/RISC-V
Embedded GCC15/bin`; override it for your install. The tool prefix
(`riscv-wch-elf-` vs `riscv32-wch-elf-`; MounRiver renamed it at GCC15) is
probed automatically, so `MRS_TOOLCHAIN` alone switches toolchains.

The WCH fork matters: the closed-source CH59x BLE library is compiled with
WCH's `WCH-Interrupt-fast` (HPE) interrupt semantics; with mainline GCC or
`INT_SOFT=1` the link succeeds but the image is silently dead (no IRQs
delivered). The Makefile enforces `INT_SOFT=0` and documents the bench
matrix behind this.

The OpenBoot **bootloader** builds stay pinned to GCC12 regardless of
`MRS_TOOLCHAIN` (upstream measured erratic GCC15 bootloader behavior;
`OPENBOOT_TOOLCHAIN` + a hard gate in the Makefile enforce this).

## Build

Prerequisites: GNU Make >= 4.3, Python 3 (slot-geometry derivation; plus
`pyserial` for `make update`), `git submodule update --init --recursive`
(the OpenBoot bootloader and its SDK pins), and a Rust toolchain for the
`openboot` CLI (updates/bundles only).

```bash
make              # default board, slot A -> build/opencontroller-ch592-slotA/...
make SLOT=B       # default board, slot B -> build/opencontroller-ch592-slotB/...
make bundle       # both slots -> build/opencontroller-ch592.obb
make factory      # blessed whole-chip image (bootloader + slot A + record)

# MK65MX Wireless: CH592 UART1 on PA8/PA9 and factory-ROM RF identity
make BOARD=mk65mx-wireless-ch592
make BOARD=mk65mx-wireless-ch592 bundle
make BOARD=mk65mx-wireless-ch592 factory
make clean
```

The MK65MX profile takes its 2.4 GHz keyboard identity from the CH592 factory
ROM. Switching an already-paired module from the default fixed identity to
this profile requires clearing and re-pairing the receiver bond.

Board profiles own the hardware-dependent knobs below; each is validated at
parse time, so a missing or non-boolean value fails the build rather than
producing a wrong image.

| Knob | `opencontroller-ch592` | `mk65mx-wireless-ch592` | Meaning |
|---|---|---|---|
| `KBD_UART1_REMAP` | 1 | 0 | 1 = UART1 on PB12/PB13, 0 = silicon-default PA8/PA9 |
| `KBD_FACTORY_MAC` | 0 | 1 | 1 = 2.4 GHz identity from the CH592 factory ROM |
| `KBD_DCDC_ENABLE` | 1 | 1 | 1 = run the core from the DC-DC converter, 0 = LDO |

`KBD_DCDC_ENABLE` is a **hardware** claim, not a preference: the board must
populate the CH592 DC-DC inductor. `PWR_DCDCCfg` declines only on silicon
that cannot do DC-DC at all (`ROM_CFG_ADR_HW` bit 13); it cannot tell that a
PCB is missing the part, and enabling it there collapses the core supply.
Both current profiles enable it (bench-measured 2026-09-02: 5.03 mA vs
7.17 mA active, −29.8%). A deliberate A/B override is
`make KBD_DCDC_ENABLE=0|1`, which passes the profile validation and is
reported by `print-board-config`; passing it through `EXTRA_CFLAGS` is
rejected at parse time because that path would silently diverge from the
reported configuration.

The size check enforces the 216 KiB slot capacity (see BOOT.md).

## Boot chain and flashing

The application runs under the [OpenBoot](https://github.com/openkeyboard-org/OpenBoot)
A/B bootloader: OpenBoot owns flash `0x0000..0x1FFF`, the app links at a
slot base (`0x2000` / `0x39000`), and updates travel over the module's own
UART via the OBP protocol. **`firmware/BOOT.md` is the authoritative
reference** — flash/RAM maps, `A6 81` bootloader entry, update and factory
flows, and recovery/revert procedures.

```bash
make flash-factory KBD_PROBE=<serial>   # whole-chip factory install via SWD
make update OB_PORT=/dev/serial/by-id/<probe-cdc>   # A/B update over UART
```

`make update` follows the selected board profile's UART pins: the default
board uses PB12/PB13; for the MK65MX profile pass
`BOARD=mk65mx-wireless-ch592` and connect the serial bridge to PA8/PA9.

Flashing uses [minichlink](https://github.com/cnlohr/ch32fun) (from
`PATH`, or `make MINICHLINK=/path/to/minichlink`) with a WCH-LinkE probe.

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
| `KBD_PAIR_DUTY_CYCLE` | 1 | bonded-reconnect search listens only in a window after each beacon (power MR4) |
| `KBD_PAIR_RX_WINDOW_TICKS` | 6 | that window, in 625 µs ticks (3.75 ms); the OpenDongle camp reply completes at beacon + ~2.7 ms, so values below 5 miss it (bench 2026-09-04) |

Bench-only interop probes (`STOCK_SUPPRESS_RESPONSES`,
`STOCK_PARK_AFTER_CATCH`, `STOCK_SEED_BOND`, …) default off via `#ifndef`
fallbacks in `src/rf_task.c`. Enable one with, for example,
`make EXTRA_CFLAGS="-DSTOCK_SUPPRESS_RESPONSES=1"`; pass multiple `-D`
options inside the quoted `EXTRA_CFLAGS` value to enable several at once.

The BLE library defaults to the submodule-provided V1.4.2 triple; other
versions are drop-in directories selected with `make BLE_LIB_DIR=...`.
