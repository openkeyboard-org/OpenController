/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bench diagnostics for a KBD_BLE build, in a 40-byte block pinned at
 * 0x20005A00 (ld/ch592f.ld), so one WCH-Link read finds them:
 *
 *   minichlink -r + 0x20005A00 40
 *
 * Offsets (little endian):
 *   0x00 u32 magic        'BLED' (0x44454C42) once filled this boot
 *   0x04 u16 stack_peak   bytes of the 2 KiB stack ever used
 *   0x06 u16 heap_peak    bytes of MEM_BUF up to the highest one ever
 *                         written; 0xFFFF for all of it, which is also
 *                         what a library that clears its heap shows
 *   0x08 u32 tmos_max     longest single TMOS_SystemProcess() pass on a
 *                         BLE boot, in 32 kHz RTC ticks (LESC key
 *                         generation lands in one; the WWDG allows ~0.56 s).
 *                         Meaningful only without KBD_BLE_SLEEP: with it, a
 *                         pass includes the sleep (the WWDG is frozen then)
 *   0x0C u32 pair_ticks   last pairing, start to completion, RTC ticks
 *   0x10 u16 connects     0x12 u16 disconnects
 *   0x14 u16 pair_ok      0x16 u16 pair_fail
 *   0x18 u16 reports_sent 0x1A u16 reports_dropped (aged out of the queue)
 *   0x1C u16 conn_interval (1.25 ms units), 0x1E u16 conn_latency,
 *   0x20 u16 conn_timeout (10 ms units): the last parameters in force
 *   0x22 u8  last_disconnect_reason (HCI)
 *   0x23 u8  last_pair_status (SMP; 0 = success)
 *   0x24 u8  adv_running  0 none, 1 pairing, 2 reconnect fast, 3 slow
 *   0x25 u8  profile      0x26 u8 generation
 *   0x27 u8  flags        bit 0 linked, bit 1 encrypted, bit 2 pairing
 *
 * Built only with RF_DIAG_COUNTERS (the bench default); the hooks compile
 * to nothing otherwise.
 */
#ifndef BLE_DIAG_H
#define BLE_DIAG_H

#include <stdint.h>

#ifndef RF_DIAG_COUNTERS
#define RF_DIAG_COUNTERS 1
#endif

typedef struct {
    uint32_t magic;
    uint16_t stack_peak;
    uint16_t heap_peak;
    uint32_t tmos_max;
    uint32_t pair_ticks;
    uint16_t connects;
    uint16_t disconnects;
    uint16_t pair_ok;
    uint16_t pair_fail;
    uint16_t reports_sent;
    uint16_t reports_dropped;
    uint16_t conn_interval;
    uint16_t conn_latency;
    uint16_t conn_timeout;
    uint8_t  last_disconnect_reason;
    uint8_t  last_pair_status;
    uint8_t  adv_running;
    uint8_t  profile;
    uint8_t  generation;
    uint8_t  flags;
} ble_diag_t;

#define BLE_DIAG_FLAG_LINKED    0x01u
#define BLE_DIAG_FLAG_ENCRYPTED 0x02u
#define BLE_DIAG_FLAG_PAIRING   0x04u

#if RF_DIAG_COUNTERS

extern volatile ble_diag_t ble_diag;

/* First thing in main(): paint the free stack and MEM_BUF so later scans
 * can tell how much of each was ever used. */
void BleDiag_Paint(void);
/* After CH59x_BLEInit. */
void BleDiag_Init(uint8_t profile, uint8_t generation);
/* Every main-loop pass; rescans the paint about every 100 ms. */
void BleDiag_Service(void);
/* The time one TMOS_SystemProcess() pass started (RtcClock_Now()). */
void BleDiag_NoteTmosPass(uint32_t started);

#define BLE_DIAG(stmt) do { stmt; } while (0)

#else

#define BleDiag_Paint() ((void)0)
#define BleDiag_Init(profile, generation) ((void)0)
#define BleDiag_Service() ((void)0)
#define BleDiag_NoteTmosPass(started) ((void)(started))
#define BLE_DIAG(stmt) do { } while (0)

#endif

#endif
