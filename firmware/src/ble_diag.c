/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bench diagnostics for a KBD_BLE build; see ble_diag.h.
 */

#include <stddef.h>

#include "CONFIG.h"
#include "HAL.h"
#include "ble_diag.h"
#include "rtc_clock.h"

#if RF_DIAG_COUNTERS

_Static_assert(sizeof(ble_diag_t) == 40, "ble_diag layout is read by address");
_Static_assert(offsetof(ble_diag_t, tmos_max) == 0x08, "ble_diag layout");
_Static_assert(offsetof(ble_diag_t, connects) == 0x10, "ble_diag layout");
_Static_assert(offsetof(ble_diag_t, last_disconnect_reason) == 0x22, "ble_diag layout");
_Static_assert(offsetof(ble_diag_t, flags) == 0x27, "ble_diag layout");

#define BLE_DIAG_MAGIC 0x44454C42u   /* "BLED" */
#define PAINT          0xA5C3A5C3u
/* Leave this much below main()'s own frame unpainted: the painting loop
 * and its callers are still using it. */
#define STACK_PAINT_MARGIN 128u

/* NOLOAD, at a fixed address: not cleared by the startup code. */
volatile ble_diag_t ble_diag __attribute__((section(".diag_ble")));

extern uint32_t _susrstack[];
extern uint32_t _eusrstack[];

static uint32_t last_scan;

void BleDiag_Paint(void)
{
    uint32_t *top = (uint32_t *)((uintptr_t)__builtin_frame_address(0) - STACK_PAINT_MARGIN);

    for (uint32_t *p = _susrstack; p < top; p++) {
        *p = PAINT;
    }
    /* MEM_BUF is still all zero from .bss here; the library takes it in
     * CH59x_BLEInit. */
    for (uint32_t i = 0; i < BLE_MEMHEAP_SIZE / 4u; i++) {
        MEM_BUF[i] = PAINT;
    }
}

void BleDiag_Init(uint8_t profile, uint8_t generation)
{
    tmos_memset((void *)&ble_diag, 0, sizeof(ble_diag));
    ble_diag.profile = profile;
    ble_diag.generation = generation;
    ble_diag.magic = BLE_DIAG_MAGIC;
    last_scan = RtcClock_Now();
}

static void scan(void)
{
    const uint32_t *p;
    uint32_t used;

    /* The stack grows down from _eusrstack: the lowest overwritten word
     * marks the deepest it has been. */
    for (p = _susrstack; p < _eusrstack && *p == PAINT; p++) {
    }
    used = (uint32_t)((uintptr_t)_eusrstack - (uintptr_t)p);
    if (used > ble_diag.stack_peak) {
        ble_diag.stack_peak = (uint16_t)used;
    }

    /* The library's heap: the highest overwritten word. 0xFFFF means all
     * of it, which is also what a library that clears its heap at init
     * would show: then this measure tells nothing. */
    p = &MEM_BUF[BLE_MEMHEAP_SIZE / 4u];
    while (p > MEM_BUF && p[-1] == PAINT) {
        p--;
    }
    used = (uint32_t)((uintptr_t)p - (uintptr_t)MEM_BUF);
    if (used >= BLE_MEMHEAP_SIZE) {
        ble_diag.heap_peak = 0xFFFFu;
    } else if (used > ble_diag.heap_peak) {
        ble_diag.heap_peak = (uint16_t)used;
    }
}

void BleDiag_Service(void)
{
    uint32_t now = RtcClock_Now();

    if ((uint32_t)(now - last_scan) < RTC_CLOCK_TICKS_US(100000u)) {
        return;
    }
    last_scan = now;
    scan();
}

void BleDiag_NoteTmosPass(uint32_t started)
{
    uint32_t ticks = RtcClock_Now() - started;

    if (ticks > ble_diag.tmos_max) {
        ble_diag.tmos_max = ticks;
    }
}

#endif
