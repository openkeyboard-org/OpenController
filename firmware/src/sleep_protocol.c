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
}

sleep_proto_action_t SleepProtocol_OnFrame(sleep_proto_t *st, uint8_t cmd,
                                           uint8_t sub, uint8_t openboot_pending)
{
    if (cmd == SLEEP_PROTO_CMD_A6 && sub == SLEEP_PROTO_SUB_UNLOCK) {
        /* Negotiate/re-negotiate v1. Idempotent; a re-request also cancels a
         * still-pending sleep (it is a new meaningful command). */
        st->unlocked = 1;
        st->sleep_pending = 0;
        return SLEEP_PROTO_SEND_READY;
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

    /* Any other real command frame (A6 !=54/56, A1 HID, A9 name -- including
     * A6 81 OTA, which additionally wins via its own latch) cancels a pending
     * sleep before it is serviced: last meaningful command wins. The host
     * ACK (0x61) and anything unrecognized leave the state untouched. */
    if (cmd == 0xA6u || cmd == 0xA1u || cmd == 0xA9u) {
        if (st->sleep_pending) {
            st->sleep_pending = 0;
            return SLEEP_PROTO_CANCEL;
        }
    }
    return SLEEP_PROTO_NONE;
}
