/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure (HAL-free) formatter for the UART diag-dump frame, host-tested.
 *
 *   A6 71 17   host: dump diag counters (bench-only; RF_DIAG_COUNTERS builds)
 *   5D <len> <payload...> <chk>   module reply; chk = sum of ALL preceding
 *              bytes (5D, len, payload) & 0xFF. len == 0 => refused (CONNECTED).
 *   A6 72 18   host: zero the dumped counters (ll_boot_count kept)
 *
 * Payload v2, little-endian, fixed order (DIAG_PAYLOAD_LEN bytes):
 *   u8  version(=2)  u8 rf_state
 *   u32 ll_boot_count  u32 rf_pair_bcast_count  u32 rf_valid_rx_count
 *   u32 entered_connected_count  u32 rf_config_count  u32 pwr_pair_rx_off_count
 *   u32 pwr_wfi_count
 *   u16 pwr_sleep_attempt  u16 pwr_sleep_entered  u16 pwr_sleep_aborted
 *   u16 pwr_wake_gpio  u16 pwr_wake_rtc
 *   u8  pwr_last_abort_reason  u8 rf_last_config_status  u8 rf_last_rx_status
 *   u8  rf_last_tx_status
 *   u32 ll_drop_count
 *   -- v2 additions (crash forensics; raw SRAM records written by startup /
 *      src/fault_handler.S, persistent across warm resets) --
 *   u8  boot_reset_status   R8_RESET_STATUS at boot; & 0x07: 0 SW, 1 power-on,
 *                           2 watchdog (WTR), 3 manual, 5 power-on-in-sleep
 *   u8  fault_marker        0xDE HardFault, 0xDF NMI, else none recorded
 *   u32 fault_mepc  u32 fault_mcause  u32 fault_mtval
 *   u16 pwr_loop_passes    main-loop passes (liveness; frozen after a WWDG boot)
 *   u8  pwr_loop_stage     last stage reached (forensics build; 0 otherwise)
 */
#ifndef DIAG_FRAME_H
#define DIAG_FRAME_H
#include <stdint.h>

#define DIAG_SUB_DUMP        0x71u
#define DIAG_SUB_ZERO        0x72u
#define DIAG_FRAME_HEADER    0x5Du
#define DIAG_PAYLOAD_VERSION 2u
#define DIAG_PAYLOAD_LEN     65u
#define DIAG_FRAME_MAX       (DIAG_PAYLOAD_LEN + 3u)   /* hdr + len + payload + chk */

typedef struct {
    uint8_t  rf_state;
    uint32_t ll_boot_count, rf_pair_bcast_count, rf_valid_rx_count;
    uint32_t entered_connected_count, rf_config_count, pwr_pair_rx_off_count;
    uint32_t pwr_wfi_count;
    uint16_t pwr_sleep_attempt, pwr_sleep_entered, pwr_sleep_aborted;
    uint16_t pwr_wake_gpio, pwr_wake_rtc;
    uint8_t  pwr_last_abort_reason, rf_last_config_status, rf_last_rx_status;
    uint8_t  rf_last_tx_status;
    uint32_t ll_drop_count;
    uint8_t  boot_reset_status, fault_marker;
    uint32_t fault_mepc, fault_mcause, fault_mtval;
    uint16_t pwr_loop_passes;
    uint8_t  pwr_loop_stage;
} diag_snapshot_t;

/* Serialise `snap` into `out` (>= DIAG_FRAME_MAX bytes). Returns frame length. */
uint8_t DiagFrame_Format(uint8_t *out, const diag_snapshot_t *snap);
/* The refused/empty frame (5D 00 5D). Returns 3. */
uint8_t DiagFrame_FormatEmpty(uint8_t *out);

#endif
