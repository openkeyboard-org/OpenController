/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef KEYBOARD_UART_H
#define KEYBOARD_UART_H

#include <stdint.h>

/* Idle-state low-power wait (power ladder MR2): interrupt-driven UART RX +
 * the flash-off WFE idle site (idle_wait_event's split form) in
 * Main_Circulation. Name kept as KBD_IDLE_WFI for the A/B knob and the
 * pwr_wfi_count counter; the mechanism is WFE, not a plain WFI (see main.c).
 * Default on; disable for a pure-polling baseline with
 * EXTRA_CFLAGS=-DKBD_IDLE_WFI=0. */
#ifndef KBD_IDLE_WFI
#define KBD_IDLE_WFI 1
#endif

/* Host wake (see host_wake.h): the module raises CHWAKE before every frame it
 * originates, for a host MCU that cannot receive while it sleeps. Board-owned
 * (the boards/ profiles); the Makefile validates it and passes it here. */
#ifndef KBD_HOST_WAKE
#define KBD_HOST_WAKE 0
#endif
/* Time from CHWAKE high to the first byte. It covers the host's wake from
 * deep sleep, clock restore and USART re-enable: the STM32U0 needs about
 * 13 us to leave STOP 2 plus its PLL relock, so 1 ms leaves a wide margin.
 * Keychron's CKBT51 radio starts sending ~200 us after its wake pin. */
#ifndef KBD_HOST_WAKE_GUARD_US
#define KBD_HOST_WAKE_GUARD_US 1000u
#endif
/* The host ACKs from its main loop, which an LED flush can hold for ~9 ms;
 * QMK uses the same 20 ms for the frames it sends us. */
#ifndef KBD_HOST_WAKE_ACK_TIMEOUT_US
#define KBD_HOST_WAKE_ACK_TIMEOUT_US 20000u
#endif
#ifndef KBD_HOST_WAKE_ATTEMPTS
#define KBD_HOST_WAKE_ATTEMPTS 3u
#endif
#if KBD_HOST_WAKE
#include "host_wake.h"
/* Longest one host-wake frame can keep TX busy: the guard, every attempt
 * timing out, then the quarantine (two ACK timeouts from the last copy, one
 * of which overlaps the last attempt's timeout). */
#define KBD_HOST_WAKE_FRAME_MAX_US \
    (KBD_HOST_WAKE_GUARD_US + (KBD_HOST_WAKE_ATTEMPTS + 1u) * KBD_HOST_WAKE_ACK_TIMEOUT_US)
/* ... and everything that can be pending at once: the queue, an LED value on
 * the wire and its replacement in the slot. A drain that must not abandon
 * host-wake frames budgets for this. */
#define KBD_HOST_WAKE_BACKLOG_MAX_US \
    ((HOST_WAKE_QUEUE_SIZE + 2u) * KBD_HOST_WAKE_FRAME_MAX_US)
#else
#define KBD_HOST_WAKE_FRAME_MAX_US 0u
#define KBD_HOST_WAKE_BACKLOG_MAX_US 0u
#endif

typedef void (*keyboard_uart_frame_cb_t)(uint8_t cmd, uint8_t sub,
                                         const uint8_t *payload, uint8_t len);

void KeyboardUart_Init(void);
void KeyboardUart_SetFrameCallback(keyboard_uart_frame_cb_t cb);
void KeyboardUart_Poll(void);

void KeyboardUart_SendAck(void);
/* Module-originated frames. A KBD_HOST_WAKE build queues them behind CHWAKE
 * (see host_wake.h) and KeyboardUart_Service() sends them; otherwise they go
 * straight to the UART as before. */
void KeyboardUart_SendStatus(uint8_t sub);
void KeyboardUart_SendBattery(uint8_t percent);
void KeyboardUart_SendLed(uint8_t led_mask);

/* Advance host wake: drive CHWAKE, send the next frame after the guard time,
 * retry it without an ACK. Call every main-loop pass. A no-op without
 * KBD_HOST_WAKE. */
void KeyboardUart_Service(void);

/* Returns 1 (and clears it) if an LED state was abandoned unacknowledged with
 * nothing newer behind it; the caller should offer the LED state again.
 * Always 0 without KBD_HOST_WAKE. */
uint8_t KeyboardUart_TakeLedLost(void);

/* Nonzero when no host-wake frame is queued or awaiting its ACK (always,
 * without KBD_HOST_WAKE). While zero the main loop must not idle: the guard
 * and ACK timeouts are polled. */
uint8_t KeyboardUart_HostWakeIdle(void);

/* Nonzero when the TX FIFO is empty AND the transmitter shift register has
 * drained - i.e. every byte handed to the UART has physically left the wire.
 * Host-wake frames still queued are not counted. */
uint8_t KeyboardUart_FifoIdle(void);

/* KeyboardUart_FifoIdle() and no host-wake frame queued or awaiting its ACK. */
uint8_t KeyboardUart_TxIdle(void);

/* Send a pre-formatted frame (diag dump). Per-byte bounded like the other
 * senders; returns bytes actually queued. */
uint8_t KeyboardUart_SendRaw(const uint8_t *buf, uint8_t len);

/* Nonzero when no received byte is waiting anywhere on the RX path: ring
 * buffer empty (KBD_IDLE_WFI builds), parser between frames, no latched
 * line error. Hardware FIFO state is deliberately NOT included - the WFI
 * site re-checks R8_UART1_RFC itself under masked IRQs. */
uint8_t KeyboardUart_RxQuiet(void);

/* Returns 1 (and clears the latch) if ANY byte was taken from the UART since
 * the last call -- including discarded/unframed bytes. Main-loop context. */
uint8_t KeyboardUart_TakeRxActivity(void);

#endif
