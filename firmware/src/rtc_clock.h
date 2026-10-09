/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Monotonic time for the application's bounded waits, on RTC32K.
 *
 * Why not SysTick: on the bench (2026-10-05) SysTick was found completely
 * cleared in the running application (CTLR, SR, CNT and CMP all read zero
 * over SWD), although CH59x_BLEInit programs and starts it. A host-wake guard
 * timed by it never ended and held CHWAKE high, and the bootloader-entry
 * drain never expired. A cold power-up ran normally at first. The cause is
 * not established; the best fit is that the module's deep sleep, which it
 * enters routinely once auto-sleep is armed, does not preserve SysTick and
 * nothing restarts it. RTC32K is the clock the RF schedule runs on and keeps
 * counting through every idle and sleep state.
 *
 * Main-loop context only (the extension state is not interrupt-safe).
 * Include HAL.h first, for FREQ_RTC.
 */
#ifndef RTC_CLOCK_H
#define RTC_CLOCK_H

#include <stdint.h>

/* Microseconds to RTC ticks, rounded up and one tick added: a wait starts at
 * an arbitrary point within its first tick and must never come up short. */
#define RTC_CLOCK_TICKS_US(us) \
    ((uint32_t)((((uint64_t)(us) * FREQ_RTC) + 999999u) / 1000000u) + 1u)

/* RTC32K reads can step back by a few ticks (the CH59x quirk rf_task.c and
 * power_sleep.c also allow 1024 ticks for). */
#define RTC_CLOCK_BACKSTEP 1024u

typedef struct {
    uint8_t  started;
    uint32_t last_raw;  /* newest raw value accepted */
    uint32_t elapsed;   /* ticks accumulated since the first sample */
} rtc_clock_t;

/* Fold one raw counter sample into the clock and return the elapsed ticks.
 * The counter counts modulo `modulus`; a modular step of `backstep` ticks or
 * less backwards, across the wrap or not, is a stale read and adds nothing.
 * Pure: native-tested. */
uint32_t RtcClock_Advance(rtc_clock_t *clk, uint32_t raw, uint32_t modulus,
                          uint32_t backstep);

/* RTC ticks since the first call, never running backwards, wrap-free for
 * about 36 hours (2^32 ticks); compare with unsigned subtraction. */
uint32_t RtcClock_Now(void);

#endif
