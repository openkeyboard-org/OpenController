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
 *   A6 57 FD   host: arm AUTO-sleep (MR7; no-op unless v1 was unlocked). The
 *              module may then deep-sleep on its own whenever idle; the host
 *              must preamble the first frame after any silence with a NULL.
 *              Stock meaning of A6 57 is "enable auto-sleep, 2.4 GHz"; this
 *              is that, behind the capability gate.
 *
 * Unlock is boot-scoped and persists across transport/pair/unpair commands.
 * Auto-sleep lifetime: A6 56 (re-)unlock RESETS it off (that is the disable
 * path -- stock has no disable opcode); explicit fresh pairing (A6 51/63) and
 * unpair (A6 52) clear it (user-attended, latency-sensitive); transport
 * selects (A6 11/30/31-33) PRESERVE it (a duty-cycled reconnect search or an
 * idle module is exactly where auto-sleep pays); boot clears it.
 */
#ifndef SLEEP_PROTOCOL_H
#define SLEEP_PROTOCOL_H

#include <stdint.h>

#define SLEEP_PROTO_CMD_A6       0xA6u
#define SLEEP_PROTO_SUB_UNLOCK   0x56u   /* A6 56: request protocol v1 */
#define SLEEP_PROTO_SUB_SLEEP    0x54u   /* A6 54: sleep now */
#define SLEEP_PROTO_SUB_AUTO     0x57u   /* A6 57: arm auto-sleep (2.4G) */
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
    uint8_t autosleep;       /* A6 57 armed: module may deep-sleep when idle */
} sleep_proto_t;

/* Boot reset: clears unlock, any pending sleep, and auto-sleep. */
void SleepProtocol_Reset(sleep_proto_t *st);

/* Fold one accepted UART frame into the state machine. `cmd` is the frame's
 * first byte (0xA6/0xA1/0xA9/0x61...), `sub` the A6 subcommand (ignored for
 * non-A6). `openboot_pending` vetoes new sleep arming (OTA wins). A
 * STATE-CHANGING command (A1 HID, A9 name, or an A6 transport-select /
 * pair / unpair / OTA sub) CANCELS a pending sleep -- last meaningful
 * command wins. Queries (battery/version), the reserved sleep-family subs,
 * the host ACK 0x61, and any UNRECOGNISED A6 sub are inert. Auto-sleep
 * (st->autosleep) follows the lifetime rules in the file header; the caller
 * mirrors it into the power module after every frame. */
sleep_proto_action_t SleepProtocol_OnFrame(sleep_proto_t *st, uint8_t cmd,
                                           uint8_t sub, uint8_t openboot_pending);

/* Elapsed ticks on a modular free-running counter whose reads may step
 * BACKWARD by a few ticks (the CH59x RTC32K quirk rf_task.c already tolerates
 * with a 1024-tick allowance). now < start by <= backstep_tol is ZERO elapsed,
 * not a wrap; only a near-modulus span is a true wrap. Pure, host-tested. */
uint32_t SleepProtocol_ElapsedTicks(uint32_t now, uint32_t start,
                                    uint32_t modulus, uint32_t backstep_tol);

#endif
