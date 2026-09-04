/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * Pure diag-frame formatter; see diag_frame.h. No HAL, host-tested. */
#include "diag_frame.h"

static uint8_t *put8(uint8_t *p, uint8_t v)   { *p++ = v; return p; }
static uint8_t *put16(uint8_t *p, uint16_t v) { *p++ = (uint8_t)v; *p++ = (uint8_t)(v >> 8); return p; }
static uint8_t *put32(uint8_t *p, uint32_t v) { p = put16(p, (uint16_t)v); return put16(p, (uint16_t)(v >> 16)); }

static uint8_t finish(uint8_t *out, uint8_t *end)
{
    uint8_t sum = 0;
    for (uint8_t *q = out; q < end; q++) { sum = (uint8_t)(sum + *q); }
    *end++ = sum;
    return (uint8_t)(end - out);
}

uint8_t DiagFrame_FormatEmpty(uint8_t *out)
{
    uint8_t *p = out;
    p = put8(p, DIAG_FRAME_HEADER);
    p = put8(p, 0);
    return finish(out, p);
}

uint8_t DiagFrame_Format(uint8_t *out, const diag_snapshot_t *s)
{
    uint8_t *p = out;
    p = put8(p, DIAG_FRAME_HEADER);
    p = put8(p, DIAG_PAYLOAD_LEN);
    p = put8(p, DIAG_PAYLOAD_VERSION);
    p = put8(p, s->rf_state);
    p = put32(p, s->ll_boot_count);
    p = put32(p, s->rf_pair_bcast_count);
    p = put32(p, s->rf_valid_rx_count);
    p = put32(p, s->entered_connected_count);
    p = put32(p, s->rf_config_count);
    p = put32(p, s->pwr_pair_rx_off_count);
    p = put32(p, s->pwr_wfi_count);
    p = put16(p, s->pwr_sleep_attempt);
    p = put16(p, s->pwr_sleep_entered);
    p = put16(p, s->pwr_sleep_aborted);
    p = put16(p, s->pwr_wake_gpio);
    p = put16(p, s->pwr_wake_rtc);
    p = put8(p, s->pwr_last_abort_reason);
    p = put8(p, s->rf_last_config_status);
    p = put8(p, s->rf_last_rx_status);
    p = put8(p, s->rf_last_tx_status);
    p = put32(p, s->ll_drop_count);
    p = put8(p, s->boot_reset_status);
    p = put8(p, s->fault_marker);
    p = put32(p, s->fault_mepc);
    p = put32(p, s->fault_mcause);
    p = put32(p, s->fault_mtval);
    p = put16(p, s->pwr_loop_passes);
    p = put8(p, s->pwr_loop_stage);
    return finish(out, p);
}
