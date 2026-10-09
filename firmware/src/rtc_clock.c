/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RTC_CLOCK_NATIVE_TEST
#include "CONFIG.h"
#include "HAL.h"
#endif
#include "rtc_clock.h"

uint32_t RtcClock_Advance(rtc_clock_t *clk, uint32_t raw, uint32_t modulus,
                          uint32_t backstep)
{
    uint32_t delta;

    if (!clk->started) {
        clk->started = 1;
        clk->last_raw = raw;
        return clk->elapsed;
    }
    /* The modular forward distance first, so a stale read just before the
     * wrap is seen as the small backward step it is, not a huge advance. */
    delta = (raw >= clk->last_raw) ? raw - clk->last_raw
                                   : raw + (modulus - clk->last_raw);
    if (delta != 0 && delta >= modulus - backstep) {
        return clk->elapsed;    /* up to `backstep` backwards: keep the newest */
    }
    clk->elapsed += delta;
    clk->last_raw = raw;
    return clk->elapsed;
}

#ifndef RTC_CLOCK_NATIVE_TEST
uint32_t RtcClock_Now(void)
{
    static rtc_clock_t clk;

    return RtcClock_Advance(&clk, RTC_GetCycle32k(), RTC_MAX_COUNT,
                            RTC_CLOCK_BACKSTEP);
}
#endif
