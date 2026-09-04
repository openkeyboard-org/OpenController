/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deep-sleep plumbing (power ladder MR5/MR6). Compiled only when the board
 * knob KBD_DEEP_SLEEP=1 swaps this module in for the SDK's HAL/SLEEP.c; the
 * two exported SDK symbols (CH59x_LowPower, HAL_SleepInit) live in
 * power_sleep.c. Every deep sleep runs inside the TMOS idle callback
 * (CH59x_LowPower) and returns 0 so the BLE library re-inits its baseband on
 * wake. Explicit host sleep (A6 54, MR6) is REQUESTED via
 * PowerSleep_RequestExplicit after Power_Service disconnects the link;
 * auto-sleep (A6 57, MR7) is armed via PowerSleep_SetAutosleep; the callback
 * performs both in RF IDLE and in the bonded search's radio-off slices.
 */
#ifndef POWER_SLEEP_H
#define POWER_SLEEP_H

#include <stdint.h>

/* Defined in main.c: the A6 81 OpenBoot entry latch. A pending bootloader
 * entry must veto sleep -- the update path expects a running main loop. */
uint8_t OpenBoot_EntryPending(void);

/* Host-commanded (A6 54) sleep: Power_Service calls this after the quiesce
 * (TX drained, bond flushed, radio disconnected). The sleep itself is
 * PERFORMED by the TMOS idle callback (CH59x_LowPower) so the BLE library is
 * told about it -- a deep sleep taken behind TMOS's back hangs the next RF
 * operation (WWDG reset). SleepPending() reports an armed-but-not-yet-taken
 * request (the main loop must not shallow-idle past it). */
void PowerSleep_RequestExplicit(void);
/* Withdraw an armed explicit request (any state-changing host command). */
void PowerSleep_CancelExplicit(void);
uint8_t PowerSleep_SleepPending(void);

/* MR7 auto-sleep: mirror of the reducer's autosleep flag (A6 57 arms; A6 56
 * reset, A6 51/52/63 and boot clear). Arming starts the activity holdoff. */
void PowerSleep_SetAutosleep(uint8_t on);
/* Any accepted UART frame restarts the KBD_AUTOSLEEP_HOLDOFF_MS holdoff, so
 * a host burst needs only one preamble. */
void PowerSleep_NoteActivity(void);
/* Forensics build: record the main-loop stage (no-op otherwise). */
void PowerSleep_Stage(uint8_t n);

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
