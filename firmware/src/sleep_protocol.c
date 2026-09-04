/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART sleep-protocol reducer (power ladder MR6). Pure logic, no HAL -- see
 * sleep_protocol.h for the wire protocol. Native-compiled by the host tests.
 */
#include "sleep_protocol.h"

void SleepProtocol_Reset(sleep_proto_t *st)
{
    st->unlocked = 0;
    st->sleep_pending = 0;
    st->autosleep = 0;
}

sleep_proto_action_t SleepProtocol_OnFrame(sleep_proto_t *st, uint8_t cmd,
                                           uint8_t sub, uint8_t openboot_pending)
{
    if (cmd == SLEEP_PROTO_CMD_A6 && sub == SLEEP_PROTO_SUB_UNLOCK) {
        /* Negotiate/re-negotiate v1 = RESET to the protocol baseline:
         * unlocked, no pending sleep, auto-sleep OFF. This doubles as the
         * auto-sleep disable path (stock has no disable opcode). */
        st->unlocked = 1;
        st->sleep_pending = 0;
        st->autosleep = 0;
        return SLEEP_PROTO_SEND_READY;
    }

    if (cmd == SLEEP_PROTO_CMD_A6 && sub == SLEEP_PROTO_SUB_AUTO) {
        /* Arm auto-sleep (idempotent). Gated like A6 54; a pending explicit
         * sleep is left alone (it proceeds, then auto-sleep governs). */
        if (st->unlocked) {
            st->autosleep = 1;
        }
        return SLEEP_PROTO_NONE;
    }

    /* Auto-sleep lifetime: user-attended pairing and unpair clear it (see
     * header); transport selects preserve it. Independent of sleep_pending. */
    if (cmd == SLEEP_PROTO_CMD_A6
            && (sub == 0x51u || sub == 0x52u || sub == 0x63u)) {
        st->autosleep = 0;
    }

    if (cmd == SLEEP_PROTO_CMD_A6 && sub == SLEEP_PROTO_SUB_SLEEP) {
        /* Sleep only when v1 is unlocked and no OTA entry is pending. A
         * duplicate A6 54 keeps the existing request (does not re-arm). */
        if (st->unlocked && !openboot_pending && !st->sleep_pending) {
            st->sleep_pending = 1;
            return SLEEP_PROTO_ARM;
        }
        return SLEEP_PROTO_NONE;
    }

    /* A STATE-CHANGING command cancels a pending sleep before it is serviced
     * (last meaningful command wins): the module must not sleep once the host
     * has selected a transport, (un)paired, entered OTA, or sent HID/name
     * data. Queries (battery 0x53, version 0x70), the reserved sleep-family
     * subs (0x55/0x57), the host ACK (0x61), and any UNRECOGNISED A6 sub
     * (e.g. A6 FF) are inert -- they do not defeat a requested sleep. */
    if (st->sleep_pending && SleepProtocol_IsStateChanging(cmd, sub)) {
        st->sleep_pending = 0;
        return SLEEP_PROTO_CANCEL;
    }
    return SLEEP_PROTO_NONE;
}

uint8_t SleepProtocol_IsStateChanging(uint8_t cmd, uint8_t sub)
{
    return (cmd == 0xA1u || cmd == 0xA9u)
        || (cmd == 0xA6u && (sub == 0x11u                    /* select USB */
                             || sub == 0x30u || sub == 0x31u /* select 2.4G/BT1 */
                             || sub == 0x32u || sub == 0x33u /* select BT2/BT3 */
                             || sub == 0x51u                 /* pair */
                             || sub == 0x52u                 /* unpair */
                             || sub == 0x63u                 /* factory pair */
                             || sub == 0x81u));              /* OTA (also wins) */
}

uint32_t SleepProtocol_ElapsedTicks(uint32_t now, uint32_t start,
                                    uint32_t modulus, uint32_t backstep_tol)
{
    if (now >= start) {
        return now - start;
    }
    if ((start - now) <= backstep_tol) {
        return 0;                       /* small backward sample, not a wrap */
    }
    return now + (modulus - start);     /* true counter wrap */
}
