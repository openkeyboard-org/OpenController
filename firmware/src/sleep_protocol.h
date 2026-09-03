/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART sleep-protocol reducer (power ladder MR6). Pure, HAL-free arbitration
 * + capability-gate state machine so it can be native-compiled and exercised
 * by the host test suite. The firmware calls SleepProtocol_OnFrame() at the
 * top of its UART frame dispatch; the returned action tells main.c what to
 * do (reply, arm the deferred sleep, or cancel a pending one). Wire protocol:
 *
 *   A6 56 FC   host: request sleep protocol v1 (encodes version in the sub)
 *   5B 37 92   module: v1-ready (only sent by a deep-sleep-capable build;
 *              the generic 61 0D 0A ACK precedes the handler and cannot
 *              signal capability, so the host must wait for THIS frame)
 *   A6 54 FA   host: sleep now (no-op unless v1 was unlocked)
 *
 * Unlock is boot-scoped and persists across transport/pair/unpair commands.
 */
#ifndef SLEEP_PROTOCOL_H
#define SLEEP_PROTOCOL_H

#include <stdint.h>

#define SLEEP_PROTO_CMD_A6       0xA6u
#define SLEEP_PROTO_SUB_UNLOCK   0x56u   /* A6 56: request protocol v1 */
#define SLEEP_PROTO_SUB_SLEEP    0x54u   /* A6 54: sleep now */
#define SLEEP_PROTO_STATUS_READY 0x37u   /* 5B 37: v1-ready reply */

typedef enum {
    SLEEP_PROTO_NONE = 0,    /* no protocol effect; dispatch the frame normally */
    SLEEP_PROTO_SEND_READY,  /* reply 5B 37 (v1 unlock acknowledged) */
    SLEEP_PROTO_ARM,         /* arm the deferred one-shot sleep */
    SLEEP_PROTO_CANCEL       /* a pending sleep was cancelled; dispatch normally */
} sleep_proto_action_t;

typedef struct {
    uint8_t unlocked;        /* v1 negotiated this boot */
    uint8_t sleep_pending;   /* A6 54 latched, awaiting Power_Service */
} sleep_proto_t;

/* Boot reset: clears unlock and any pending sleep. */
void SleepProtocol_Reset(sleep_proto_t *st);

/* Fold one accepted UART frame into the state machine. `cmd` is the frame's
 * first byte (0xA6/0xA1/0xA9/0x61...), `sub` the A6 subcommand (ignored for
 * non-A6). `openboot_pending` vetoes new sleep arming (OTA wins). Frames
 * other than the unlock/sleep pair CANCEL a pending sleep (last meaningful
 * command wins); the host ACK 0x61 and anything unrecognized are inert. */
sleep_proto_action_t SleepProtocol_OnFrame(sleep_proto_t *st, uint8_t cmd,
                                           uint8_t sub, uint8_t openboot_pending);

#endif
