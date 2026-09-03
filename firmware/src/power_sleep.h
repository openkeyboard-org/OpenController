/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deep-sleep plumbing (power ladder MR5/MR6). Compiled only when the board
 * knob KBD_DEEP_SLEEP=1 swaps this module in for the SDK's HAL/SLEEP.c; the
 * two exported SDK symbols (CH59x_LowPower, HAL_SleepInit) live in
 * power_sleep.c. Explicit host-commanded sleep (PowerSleep_ExplicitOnce,
 * A6 54) is LIVE from MR6; autonomous sleep (MR7) is armed by A6 57 via
 * PowerSleep_SetAutosleep and runs from two sites: the main loop in RF IDLE
 * (PowerSleep_IdleAutosleep) and the TMOS idle callback in the bonded
 * reconnect search's radio-off slices.
 */
#ifndef POWER_SLEEP_H
#define POWER_SLEEP_H

#include <stdint.h>

/* Defined in main.c: the A6 81 OpenBoot entry latch. A pending bootloader
 * entry must veto sleep -- the update path expects a running main loop. */
uint8_t OpenBoot_EntryPending(void);

/* Host-commanded (A6 54) one-shot deep sleep, GPIO-only wake (MR6). Caller
 * has already drained TX, flushed the bond, and disconnected the radio.
 * Returns 0 slept-and-woke, 3 vetoed at the boundary. */
uint32_t PowerSleep_ExplicitOnce(void);

/* MR7 auto-sleep: mirror of the reducer's autosleep flag (A6 57 arms; A6 56
 * reset, A6 51/52/63 and boot clear). Arming starts the activity holdoff. */
void PowerSleep_SetAutosleep(uint8_t on);
/* Any accepted UART frame restarts the KBD_AUTOSLEEP_HOLDOFF_MS holdoff, so
 * a host burst needs only one preamble. */
void PowerSleep_NoteActivity(void);
/* Main-loop autonomous sleep in RF IDLE: 0 slept-and-woke, 3 not applicable
 * or vetoed. Caller has checked RF IDLE / no OpenBoot / no pending explicit
 * sleep / RX quiet. */
uint32_t PowerSleep_IdleAutosleep(void);

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
