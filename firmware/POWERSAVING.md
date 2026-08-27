<!--
Copyright 2026 Eric Molitor (EMulator)
SPDX-License-Identifier: Apache-2.0
-->

# Power saving — CH592F keyboard module

Survey of power-reduction opportunities in this firmware, ordered by how much
risk each one carries for 2.4 GHz link performance. Written against `main`;
items that depend on in-flight branches say so.

**Nothing here is measured.** No current figures exist for this firmware in any
state — idle, connected-idle, or per-keystroke. Every estimate below is
reasoning from the source and the CH59x SDK, not from a bench. Take a baseline
before acting on any of it; see [Measure first](#measure-first).

## Baseline — what the firmware does today

| | State on `main` |
|---|---|
| Core clock | 60 MHz PLL, always (`src/main.c:210`), never lowered |
| Main loop | pure busy-spin (`src/main.c:191`) — TMOS, hop tick, UART poll, watchdog feed; no idle instruction anywhere |
| Sleep | `HAL_SLEEP` defaults to FALSE (`../third_party/weactstudio-wch-ble-core/Examples/CH592/ble/broadcaster/ble/HAL/include/CONFIG.h:84-86`), so `CH59x_LowPower()` returns 3 and `HAL_SleepInit()` is empty (`../third_party/weactstudio-wch-ble-core/Examples/CH592/ble/broadcaster/ble/HAL/SLEEP.c:26-75`, `../third_party/weactstudio-wch-ble-core/Examples/CH592/ble/broadcaster/ble/HAL/SLEEP.c:86-97`) |
| UART RX | polled. `KeyboardUart_Init` calls `UART1_DefInit` (`src/keyboard_uart.c:73-84`), which sets `R8_UART1_IER = RB_IER_TXD_EN` (`../third_party/weactstudio-wch-ble-core/Examples/CH592/ble/broadcaster/StdPeriphDriver/CH59x_uart1.c:24-31`); no RX interrupt exists |
| 32 kHz source | internal RC — SDK defaults `CLK_OSC32K` to 1 (`../third_party/weactstudio-wch-ble-core/Examples/CH592/ble/broadcaster/ble/HAL/include/CONFIG.h:123-125`) and the application never overrides it, so the LSE crystal path is already unpowered |
| Peripheral gating | untouched; `R32_SLEEP_CONTROL` is never written |
| DC-DC | off on `main`; enabled for the MK65MX profile by PR #7 |

The core spins at 60 MHz doing nothing for the overwhelming majority of each
28-RTC-tick (~0.87 ms) poll interval. That idle time is the prize.

## The constraint everything hinges on

`RF_ConnectedTick()` is a **polled** hop scheduler. It reads the RTC counter
(`src/rf_task.c:346`) on each call and compares elapsed ticks against
`rf_conn_interval`; nothing interrupts it into running. The consequence is
already documented in the UART TX timeout rationale
(`src/keyboard_uart.c:12`): a stalled main loop stalls `RF_ConnectedTick`,
which stalls the time-based hop, which drops the RF link with no recovery.

So any sleep taken while connected **must** guarantee a wake before the next
hop deadline. That single fact orders everything below.

## Tier A — no link risk, do first

### Park unused GPIOs

WCH's own examples park every unused pin (`GPIOx_ModeCfg(GPIO_Pin_All,
GPIO_ModeIN_PU)`) before sleeping; this firmware parks none. A floating CMOS
input can sit mid-rail and burn crossbar current continuously.

One concrete instance, on the MK65MX profile from PR #6: its default-pin branch
in `KeyboardUart_Init` parks PB12 **and** PB13 as `GPIO_ModeIN_Floating`. PB13
is fine — CHWAKE drives it from the board — but PB12 is genuinely floating and
wants a pull. Any pull added there must not disturb PB13.

### Power down unused clock units

`PWR_UnitModCfg` can drop `UNIT_SYS_LSE`, which is dead weight given the
internal-RC RTC selection above.

### Gate unused peripheral clocks

`PWR_PeriphClkCfg` for UART0/2/3, TMR1–3, SPI0, PWMX and USB — none of which
this firmware uses.

Read the implementation before budgeting a saving for this: it writes
`R32_SLEEP_CONTROL`, so it gates clocks **during sleep only**. On its own,
with no sleep anywhere, it saves nothing. It is a prerequisite for Tier B and
Tier C, not a win in its own right.

## Tier B — the best effort-to-benefit ratio: sleep when not connected

`RF_ConnectedTick()` returns immediately at its state gate
(`src/rf_task.c:1152`) unless `rf_state == RF_STATE_CONNECTED`. In idle,
disconnected, and bonded-but-unreachable states there is therefore **no hop
deadline to miss**, and sleep cannot degrade link performance — by
construction, not by measurement.

Today all of those states burn the same full-speed 60 MHz spin as an active
link. A keyboard plausibly spends most of its life there: host asleep, lid
shut, dongle unplugged.

Requirements:

- a UART wake source, so `A6 30` / `A6 51` / `A6 81` still land —
  `PWR_PeriphWakeUpCfg` supports UART wake; a periodic RTC wake is the
  fallback;
- `HAL_SLEEP` enabled, which is what makes `CH59x_LowPower()` more than a
  stub.

This is the only large saving that needs no re-validation of connected-mode
timing.

## Tier C — idle the core between polls

`LowPower_Idle()` is `__WFI()` with the flash powered down. Dropping it into
`Main_Circulation` as-is **will** break the link. Two blockers, both fixable:

1. **UART RX is polled.** A sleeping core misses bytes outright. Needs
   `RB_IER_RECV_RDY` plus an ISR, or a drain-on-wake. The existing parser is
   already fed one byte at a time, so this part is mechanical.
2. **The hop scheduler is polled.** Needs a guaranteed wake tied to the next
   hop deadline — an RTC trigger or a spare timer — so `__WFI()` can never
   overshoot it.

Also note `LowPower_Idle()` sets `R8_FLASH_CTRL = 0x04`. `Main_Circulation` is
`__HIGH_CODE` (`src/main.c:189`) and therefore RAM-resident, but its callees
are not, so every wake would pay a flash re-enable. Either use a bare
`__WFI()` without the flash-off, or move the hot path into RAM.

Done properly this preserves link performance, but it changes the timing the RF
stack was bench-validated against. It needs the full bench matrix re-run, not a
smoke test.

## Tier D — these cost performance

Recorded so they can be ruled out deliberately rather than rediscovered.

**Lowering the core clock.** `RF_TURNAROUND_COUNT` is 6000 counts
(`src/rf_task.c:223`), ~100 µs at 60 MHz, and the poll response must land
inside the dongle's post-poll RX window. Every calibrated count shifts at
32 MHz. High risk, modest gain.

**Deep sleep between polls while connected.** The textbook keyboard approach
and the largest theoretical saving. HSE/PLL restart latency eats into an
~0.87 ms budget, and the hop scheduler would have to become deadline-driven
rather than polled. Large rework.

**Reducing `HID_RESEND_COUNT`** (`src/rf_task.c:216`). TX dominates radio
energy, so this is a real lever — and a real risk to key delivery, which is
why it sits in this tier.

Related, and worth writing into the QMK host contract: PR #5 *raises* average
TX energy. After it, every UART `A1` submission re-arms six LEN=10 responses,
including duplicate payloads that previously cost nothing. That is bounded
while a host submits on change plus resync barriers. A host that submitted at
its matrix scan rate would make the radio send LEN=10 on every poll instead of
LEN=1 keepalives.

## Measure first

Priorities here are inferred from source, and the ordering could be wrong. A
baseline of idle, connected-idle and per-keystroke draw would show whether
Tier B is worth ten times Tier A or the reverse.

This applies to work already in flight too: PR #7 enables the DC-DC converter
for the MK65MX profile on the strength of the datasheet, with no bench figures
behind it.
