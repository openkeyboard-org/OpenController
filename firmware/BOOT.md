<!--
Copyright 2026 Eric Molitor (EMulator)
SPDX-License-Identifier: Apache-2.0
-->

# Boot architecture: OpenBoot on the CH592F keyboard module

The application runs under [OpenBoot](https://github.com/openkeyboard-org/OpenBoot)
(`third_party/openboot`, pinned; see `Makefile:OPENBOOT_REVISION`), an 8 KiB
A/B-slot bootloader speaking the OBP protocol over the module's existing
UART1 pins. This replaces the reverse-engineered stock vendor chain (app
@`0x1000`, trampoline @`0x0`, vendor stage-1 @`0x6D000`, IAP staging) that the
firmware booted under through the `pre-openboot-stock-chain` tag.

## Flash map (CH592, 448 KiB)

| Range | Owner |
|---|---|
| `0x00000..0x01FFF` | OpenBoot (8 KiB budget; ~5 KiB used) |
| `0x02000..0x37FFF` | Slot A application image (216 KiB capacity) |
| `0x38000..0x38FFF` | Slot A `OBR2` boot record (owns the whole erase block) |
| `0x39000..0x6EFFF` | Slot B application image |
| `0x6F000..0x6FFFF` | Slot B `OBR2` boot record |
| DataFlash `0x4000` (phys `0x74000`) | `KBD2` 2.4G bond record — **untouched by OpenBoot** (code-flash-only protocol) and by `minichlink -E` |

Each build targets one slot (`make SLOT=A|B`); the BLE library's relocations
do not survive a base shift, so slot B is a second build, never a copy.
Releases ship both as one `.obb` bundle (`make bundle`) — the device names
which slot it will accept over HELLO, and it alternates after every COMMIT.

Every image carries a 32-byte ODG2-format identity header at image offset
`0x20` — magic `ODG2`, **family `0xB2`** (OpenController keyboard; dongle
families are `0x92`/`0x70`), kind, slot base, image length. OpenBoot itself
attests only length+CRC at COMMIT, so this header plus the host-side family
check in `make update` is the only guard against cross-flashing dongle CH592
images on a shared bench.

## RAM map

| Range | Owner |
|---|---|
| `0x20000000..0x200057FF` | app data/bss (`ASSERT(_ebss <= 0x20005800)`) |
| `0x20005800..0x2000581F` | asm diag scratch: boot-phase sentinel @`+0`, reset status @`+1`, fault latches @`+4..+13` (hard-coded in `startup_CH592_phased.S` / `fault_handler.S`) |
| `0x20005820..0x200059FF` | `.diag_safe` counters (**moved from `0x20005E00` at OpenBoot adoption** — re-derive any bench-script SWD addresses) |
| `0x20005FF0..0x200067EF` | stack (2 KiB; `_eusrstack == 0x200067F0` asserted) |
| `0x200067F0..0x200067FF` | OpenBoot boot-request mailbox (top 16 B of SRAM, reserved) |

Boot-phase sentinel values: `0xC0..0xC5` startup phases (see
`startup_CH592_phased.S`), then `0xA0..0xA7` main() init phases (see
`main.c BOOT_PHASE`). A wedge is SWD-attributable without a debugger.

## Boot flow

Reset → OpenBoot → boot decision: RAM boot-request magic ⇒ stay in the
bootloader; otherwise the highest-generation bootable slot is launched
**immediately** (a blessed/committed device spends only milliseconds in the
bootloader — with `OB_BOOT_IMAGE_CRC=1` plus a whole-image CRC first). The
10 s idle timeout applies only when the bootloader *stays* resident: a
device parked there (boot request, or no bootable slot... none) auto-boots
the newest bootable slot after 10 s unless a HELLO arrives first (the first
successful HELLO disables the timeout for that power cycle — fail-stay).

**SysTick handoff:** OpenBoot uses SysTick for its idle timeout and stops
the counter before jumping to the app (`ob_jump_app`) — but stopping does
not clear an already-latched count-flag or the PFIC pending bit. `main.c`
clears that pending state before `CH59x_BLEInit()`: the HAL enables the
SysTick IRQ one line before disabling it in PFIC, and the inherited pending
state fires in that window into the startup's weak infinite-loop handler
(bench-diagnosed wedge; do not remove the clear). Upstream candidate:
OpenBoot could clear SR/PFIC pending at handoff.

## Entering the bootloader

`A6 81` (the stock OTA-mode command, previously a no-op) over the host UART:
the dispatch latches, and `OpenBoot_Service()` in the main loop flushes any
pending bond save, quiesces RF (`RF_Disconnect`), drains the frame's ACK
(bounded ~20 ms), masks IRQs, and calls `openboot_request_update()` — magic
`0xB007CA11` to `0x200067F0` plus a safe-access software reset. Single-shot
is fail-safe on this board: a spurious `A6 81` costs ~10 s off-air and the
idle timeout boots the application back (the bonded link then auto-recovers
via the keyboard's reconnect path once the host re-selects 2.4G — or
immediately, since the app re-enters its bonded state on boot).

## Update flows

**Bench OBP update (WCH-LinkE CDC on PB12/PB13):**

```sh
make update OB_PORT=/dev/serial/by-id/usb-wch.cn_WCH-Link_<serial>-if01
```

builds both slots, bundles, family-checks (`0xB2`), sends `A6 81`, settles
**6 s**, then drives `openboot flash <bundle> --force` (which ends by booting
the new image — no separate `boot` step). The settle matters: for ~2–4 s
after the soft reset the bootloader's UART drops or corrupts frames
(bench-measured `OB_E_CRC` reports at t<2 s, clean HELLO from ~2.3 s, with
variance); the CLI's fast HELLO retries all land inside that window
otherwise. Total idle budget is 10 s, so the settle cannot grow much more.

**Factory install (SWD, whole chip):**

```sh
make flash-factory KBD_PROBE=<WCH-LinkE serial> [ALLOW_BONDED_FLASH=1]
```

erases code flash, writes the blessed factory image at 0 (bootloader +
0x00 pad + slot-A app + slot-A record — the pad must be 0x00: programming
0xFF programs nothing on CH5xx), verifies by readback (`minichlink`'s own
verify is disabled and its error returns ignored), guards against shipping
a unit with a surviving `KBD2` bond (DataFlash outlives `-E`), and ends
with a **real power cycle** (`-kt`/`-k3`, never `-b`, which resumes from
halt without re-running the boot path).

**Recovery / revert to the stock chain:** SWD always works (UART builds
leave SWD enabled; the module is rail-powered from its probe). To revert:
`git checkout pre-openboot-stock-chain`, build, `minichlink -E`, write the
old trampoline+app layout at 0. The bond survives both directions.

**BOOT response ambiguity:** the CLI never retries BOOT — a lost response
is indistinguishable from a successful boot (and the post-reset UART
turbulence makes lost responses common). After a standalone `openboot boot`
error, check the actual state: a battery query (`A6 53`) answered means the
application is up.

## Known limits / roadmap

- QMK-host-driven updates need an embedded OBP client on the host MCU —
  out of scope for the bench bring-up; the transport and framing are ready.
- The post-reset UART turbulence window deserves an upstream look
  (OpenBoot's UART init after a soft reset from a 60 MHz application).
- Keyboard-reset link recovery: bring-up believed this failed against v0.96.x
  receivers (an "EV10 reacquire" regression). That deterministic failure was
  traced to a **debugger-measurement artifact** — reproducing/reading it over
  minichlink (SDI reset + `-r` halt/resume) induces a TMOS misaligned-load
  fault that does not occur in normal operation (see `docs/TMOS_REVIEW.md`).
  Clean USB-HID validation on the production build — a 1000-reset randomized
  warm-reset soak (SDI + true power-cycle, dwells to 15 s) — recovered
  **999/1000** with a ~2.34 s reconnect; the lone failure was a rare
  marginal-delivery blip that still reached CONNECTED, not a link-recovery
  failure. No deterministic reset-recovery defect remains.
