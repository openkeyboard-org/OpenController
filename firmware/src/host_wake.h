/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host wake over CHWAKE (KBD_HOST_WAKE). Pure logic, no HAL, so the host tests
 * native-compile it; keyboard_uart.c drives the pin and the UART from it.
 *
 * A keyboard host MCU that deep-sleeps (the MK65MX's STM32U0 in STOP 2) cannot
 * receive on its USART, so the module raises CHWAKE before it says anything:
 *
 *   1. A frame for the host (5B status, 5C battery) is queued, or the latest
 *      LED state (5A) is set.
 *   2. CHWAKE goes high and stays high for the guard time, which covers the
 *      host's wake-up, clock restore and USART re-enable.
 *   3. The frame goes out and the module waits for the host's 61 0D 0A ACK.
 *      No ACK within the timeout: resend, up to max_attempts sends, then the
 *      frame is abandoned. ACKed or abandoned, the next frame follows at once
 *      with CHWAKE still high, so a burst pays the guard only once.
 *   4. CHWAKE goes low when nothing is left.
 *
 * Bare ACKs to the host's own commands are not covered: the host is awake
 * when it sends a command and waits for that ACK.
 *
 * ACKs carry no sequence number, and the host ACKs every copy it receives.
 * One frame is outstanding at a time, so an ACK answers it, except after a
 * resend: then a late ACK for an earlier copy could retire the next frame
 * before that frame was ever received. So once a frame has been sent more
 * than once, nothing more is sent until two ACK timeouts after its last copy
 * went out, and ACKs arriving in that quarantine are discarded. This assumes
 * the host ACKs any copy within two ACK timeouts of it being sent.
 *
 * The LED state is not queued: only the newest value matters, so it lives in
 * a slot of its own that a new value overwrites. It goes ahead of queued
 * frames, but never twice in a row while frames are queued, so neither can
 * starve the other. It is never refused. If a value
 * is abandoned with nothing newer behind it, latest_lost is set so the owner
 * can arrange for it to be offered again.
 *
 * Every wait is a state checked from the main loop, never a busy wait: the
 * module keeps servicing RF and incoming commands throughout.
 */
#ifndef HOST_WAKE_H
#define HOST_WAKE_H

#include <stdint.h>

#define HOST_WAKE_QUEUE_SIZE 8u

typedef struct {
    /* Configuration, in the caller's tick unit. */
    uint32_t guard_ticks;
    uint32_t ack_timeout_ticks;
    uint8_t  max_attempts;

    uint8_t  state;       /* HOST_WAKE_STATE_* in host_wake.c */
    uint8_t  head;
    uint8_t  count;
    uint8_t  attempts;    /* sends of the outstanding frame so far */
    uint8_t  acked;       /* the host ACKed the outstanding frame */
    uint8_t  sending_latest; /* the outstanding frame is the latest slot's */
    uint8_t  last_latest; /* the previous frame sent was the latest slot's */
    uint32_t since;       /* tick at which the current wait began */
    uint8_t  cmd[HOST_WAKE_QUEUE_SIZE];
    uint8_t  val[HOST_WAKE_QUEUE_SIZE];

    uint8_t  latest_pending; /* the latest slot holds a value not yet sent */
    uint8_t  latest_lost; /* a latest-slot value was abandoned, nothing newer */
    uint8_t  latest_cmd;
    uint8_t  latest_val;
    uint8_t  out_cmd;     /* the outstanding frame, for resends */
    uint8_t  out_val;

    uint16_t dropped;     /* queued frames refused: queue full */
    uint16_t abandoned;   /* frames given up after max_attempts sends */
    uint16_t retired;     /* frames done with, ACKed or abandoned */
} host_wake_t;

typedef struct {
    uint8_t line;       /* CHWAKE level to drive from now on */
    uint8_t send;       /* 1: send cmd/val now */
    uint8_t cmd;
    uint8_t val;
} host_wake_step_t;

void HostWake_Init(host_wake_t *hw, uint32_t guard_ticks,
                   uint32_t ack_timeout_ticks, uint8_t max_attempts);

/* Queue a two-byte frame (the checksum is the sender's). 0 if the queue is
 * full; the frame is dropped and counted. */
uint8_t HostWake_Enqueue(host_wake_t *hw, uint8_t cmd, uint8_t val);

/* Set the latest-value slot (the LED state): replaces any value not yet sent,
 * and goes ahead of queued frames, though never twice in a row while frames
 * are queued. Never fails. */
void HostWake_SetLatest(host_wake_t *hw, uint8_t cmd, uint8_t val);

/* A 61 0D 0A arrived from the host. Retires the outstanding frame on the next
 * step; with nothing outstanding, or in quarantine, it is discarded. */
void HostWake_OnAck(host_wake_t *hw);

/* Advance the state machine. Call every main-loop pass; `now` is a free-
 * running counter in the configured tick unit (wrap-safe). */
host_wake_step_t HostWake_Step(host_wake_t *hw, uint32_t now);

/* 1 when nothing is queued, pending or outstanding and CHWAKE is low. */
uint8_t HostWake_Idle(const host_wake_t *hw);

#endif
