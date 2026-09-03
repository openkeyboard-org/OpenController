/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deep-sleep plumbing (power ladder MR5). Compiled only when the board knob
 * KBD_DEEP_SLEEP=1 swaps this module in for the SDK's HAL/SLEEP.c; the two
 * exported SDK symbols (CH59x_LowPower, HAL_SleepInit) live in power_sleep.c.
 * Runtime-inert: nothing arms deep sleep until the UART sleep-protocol rung
 * sets the runtime enable (or the EXTRA_CFLAGS-only bench hook is poked).
 */
#ifndef POWER_SLEEP_H
#define POWER_SLEEP_H

#include <stdint.h>

/* Defined in main.c: the A6 81 OpenBoot entry latch. A pending bootloader
 * entry must veto sleep -- the update path expects a running main loop. */
uint8_t OpenBoot_EntryPending(void);

#if KBD_SLEEP_BENCH_HOOK
/* Main-loop service for the bench hook: runs ONE 5 s deep sleep through the
 * full quiesce procedure when a request is pending, so the WWDG-in-sleep
 * question and the sleep floor can be measured. */
void PowerSleep_BenchService(void);
/* Arm one bench sleep. Driven over UART (A6 54 under the hook build) so the
 * whole experiment runs on the serial link with the meter as the witness --
 * SWD reads perturb and cannot observe a sleeping core. */
void PowerSleep_BenchRequest(void);
#endif

#endif
