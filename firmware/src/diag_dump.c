/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 * Firmware glue for the UART diag dump; the frame layout is diag_frame.c. */
#include "CONFIG.h"
#include "diag_dump.h"
#include "diag_frame.h"
#include "keyboard_uart.h"
#include "rf_task.h"

#ifndef RF_DIAG_COUNTERS
#define RF_DIAG_COUNTERS 1
#endif
#ifndef KBD_UART_DIAG_DUMP
#define KBD_UART_DIAG_DUMP RF_DIAG_COUNTERS
#endif

/* Raw diagnostic SRAM records (see src/fault_handler.S header). Persist across
 * warm resets; DiagDump_Zero clears the fault record so the next one is fresh. */
#define DIAG_RESET_STATUS   (*(volatile uint8_t  *)0x20005801u)
#define DIAG_FAULT_MARKER   (*(volatile uint8_t  *)0x20005804u)
#define DIAG_FAULT_MEPC     (*(volatile uint32_t *)0x20005808u)
#define DIAG_FAULT_MCAUSE   (*(volatile uint32_t *)0x2000580Cu)
#define DIAG_FAULT_MTVAL    (*(volatile uint32_t *)0x20005810u)

#if RF_DIAG_COUNTERS && KBD_UART_DIAG_DUMP
/* Counters owned by rf_task.c (external linkage, .diag_safe*). */
extern volatile uint32_t ll_boot_count, rf_pair_bcast_count, rf_valid_rx_count;
extern volatile uint32_t entered_connected_count, rf_config_count, pwr_pair_rx_off_count;
extern volatile uint32_t ll_drop_count;
extern volatile uint8_t  rf_last_config_status, rf_last_rx_status, rf_last_tx_status;
#if KBD_IDLE_WFI
extern volatile uint32_t pwr_wfi_count;          /* main.c */
#endif
#if KBD_DEEP_SLEEP
extern volatile uint16_t pwr_sleep_attempt, pwr_sleep_entered, pwr_sleep_aborted;
extern volatile uint16_t pwr_wake_gpio, pwr_wake_rtc;
extern volatile uint8_t  pwr_last_abort_reason;  /* power_sleep.c */
extern volatile uint16_t pwr_loop_passes;
extern volatile uint8_t  pwr_loop_stage, pwr_forensics_frozen;
#endif

void DiagDump_Send(void)
{
    uint8_t frame[DIAG_FRAME_MAX];
    uint8_t n;
    uint8_t connected = (RF_GetState() == RF_STATE_CONNECTED);
    if (connected) {
        n = DiagFrame_FormatEmpty(frame);
    } else {
        diag_snapshot_t s = {0};
        s.rf_state = RF_GetState();
        s.ll_boot_count = ll_boot_count;
        s.rf_pair_bcast_count = rf_pair_bcast_count;
        s.rf_valid_rx_count = rf_valid_rx_count;
        s.entered_connected_count = entered_connected_count;
        s.rf_config_count = rf_config_count;
        s.pwr_pair_rx_off_count = pwr_pair_rx_off_count;
#if KBD_IDLE_WFI
        s.pwr_wfi_count = pwr_wfi_count;
#endif
#if KBD_DEEP_SLEEP
        s.pwr_sleep_attempt = pwr_sleep_attempt;
        s.pwr_sleep_entered = pwr_sleep_entered;
        s.pwr_sleep_aborted = pwr_sleep_aborted;
        s.pwr_wake_gpio = pwr_wake_gpio;
        s.pwr_wake_rtc = pwr_wake_rtc;
        s.pwr_last_abort_reason = pwr_last_abort_reason;
#endif
        s.rf_last_config_status = rf_last_config_status;
        s.rf_last_rx_status = rf_last_rx_status;
        s.rf_last_tx_status = rf_last_tx_status;
        s.ll_drop_count = ll_drop_count;
        s.boot_reset_status = DIAG_RESET_STATUS;
        s.fault_marker = DIAG_FAULT_MARKER;
        s.fault_mepc = DIAG_FAULT_MEPC;
        s.fault_mcause = DIAG_FAULT_MCAUSE;
        s.fault_mtval = DIAG_FAULT_MTVAL;
#if KBD_DEEP_SLEEP
        s.pwr_loop_passes = pwr_loop_passes;
        s.pwr_loop_stage = pwr_loop_stage;
#endif
        n = DiagFrame_Format(frame, &s);
    }
    uint8_t sent = KeyboardUart_SendRaw(frame, n);
#if KBD_DEEP_SLEEP
    /* Thaw ONLY after a COMPLETE, NON-EMPTY frame actually left the UART: the
     * frozen post-watchdog record has then genuinely been read out. A refusal
     * while CONNECTED (empty 5D 00 5D) or a truncated send (host not draining,
     * SendRaw < n) must keep the record frozen so the pre-crash counters are
     * not lost unread (review findings). A6 72 is the explicit thaw. */
    if (!connected && sent == n) {
        pwr_forensics_frozen = 0;
    }
#else
    (void)sent;
#endif
}

/* Bench semantics: plain stores from main-loop context; an ISR increment
 * racing a zero can lose one count -- acceptable for a diagnostic. The reboot
 * counter is deliberately kept. */
void DiagDump_Zero(void)
{
    /* Never clear the counters or the retained fault record over a live link
     * (DiagDump_Send refuses there too): a connected host must not wipe the
     * diagnostic/forensic state (review finding). */
    if (RF_GetState() == RF_STATE_CONNECTED) {
        return;
    }
    rf_pair_bcast_count = 0; rf_valid_rx_count = 0; entered_connected_count = 0;
    rf_config_count = 0; pwr_pair_rx_off_count = 0; ll_drop_count = 0;
    rf_last_config_status = 0; rf_last_rx_status = 0; rf_last_tx_status = 0;
#if KBD_IDLE_WFI
    pwr_wfi_count = 0;
#endif
#if KBD_DEEP_SLEEP
    pwr_sleep_attempt = 0; pwr_sleep_entered = 0; pwr_sleep_aborted = 0;
    pwr_wake_gpio = 0; pwr_wake_rtc = 0; pwr_last_abort_reason = 0;
#endif
    DIAG_FAULT_MARKER = 0; DIAG_FAULT_MEPC = 0; DIAG_FAULT_MCAUSE = 0; DIAG_FAULT_MTVAL = 0;
#if KBD_DEEP_SLEEP
    pwr_loop_passes = 0; pwr_loop_stage = 0; pwr_forensics_frozen = 0;   /* resume live counting */
#endif
}
#else
void DiagDump_Send(void) { }
void DiagDump_Zero(void) { }
#endif
