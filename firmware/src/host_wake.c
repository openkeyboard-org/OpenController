/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host wake over CHWAKE: see host_wake.h. Native-compiled by the host tests.
 */
#include "host_wake.h"

enum {
    HOST_WAKE_STATE_IDLE = 0,   /* CHWAKE low, nothing pending */
    HOST_WAKE_STATE_GUARD,      /* CHWAKE high, waiting for the host to wake */
    HOST_WAKE_STATE_WAIT_ACK,   /* a frame is out, waiting for 61 0D 0A */
    HOST_WAKE_STATE_QUARANTINE, /* after a resend: late ACKs are discarded */
};

void HostWake_Init(host_wake_t *hw, uint32_t guard_ticks,
                   uint32_t ack_timeout_ticks, uint8_t max_attempts)
{
    uint8_t i;

    hw->guard_ticks = guard_ticks;
    hw->ack_timeout_ticks = ack_timeout_ticks;
    hw->max_attempts = max_attempts ? max_attempts : 1u;
    hw->state = HOST_WAKE_STATE_IDLE;
    hw->head = 0;
    hw->count = 0;
    hw->attempts = 0;
    hw->acked = 0;
    hw->sending_latest = 0;
    hw->last_latest = 0;
    hw->since = 0;
    for (i = 0; i < HOST_WAKE_QUEUE_SIZE; i++) {
        hw->cmd[i] = 0;
        hw->val[i] = 0;
    }
    hw->latest_pending = 0;
    hw->latest_lost = 0;
    hw->latest_cmd = 0;
    hw->latest_val = 0;
    hw->out_cmd = 0;
    hw->out_val = 0;
    hw->dropped = 0;
    hw->abandoned = 0;
    hw->retired = 0;
}

uint8_t HostWake_Enqueue(host_wake_t *hw, uint8_t cmd, uint8_t val)
{
    uint8_t slot;

    if (hw->count >= HOST_WAKE_QUEUE_SIZE) {
        hw->dropped++;
        return 0;
    }
    slot = (uint8_t)((hw->head + hw->count) % HOST_WAKE_QUEUE_SIZE);
    hw->cmd[slot] = cmd;
    hw->val[slot] = val;
    hw->count++;
    return 1;
}

void HostWake_SetLatest(host_wake_t *hw, uint8_t cmd, uint8_t val)
{
    hw->latest_cmd = cmd;
    hw->latest_val = val;
    hw->latest_pending = 1;
}

void HostWake_OnAck(host_wake_t *hw)
{
    if (hw->state == HOST_WAKE_STATE_WAIT_ACK) {
        hw->acked = 1;
    }
}

static uint8_t have_pending(const host_wake_t *hw)
{
    return hw->count != 0 || hw->latest_pending;
}

static host_wake_step_t transmit(host_wake_t *hw, uint32_t now)
{
    host_wake_step_t out;

    out.line = 1;
    out.send = 1;
    out.cmd = hw->out_cmd;
    out.val = hw->out_val;
    hw->attempts++;
    hw->since = now;
    hw->acked = 0;
    hw->state = HOST_WAKE_STATE_WAIT_ACK;
    return out;
}

/* The latest slot goes first, but never twice running while frames are
 * queued: steady status traffic cannot starve the LED, and an LED value that
 * keeps being re-offered (a host that stopped ACKing, RF_LedResync, the next
 * receiver report) cannot starve the queue. Taking its value clears it, so a
 * change made while this copy is on the wire sets it again. */
static host_wake_step_t send_next(host_wake_t *hw, uint32_t now)
{
    if (hw->latest_pending && !(hw->last_latest && hw->count != 0)) {
        hw->out_cmd = hw->latest_cmd;
        hw->out_val = hw->latest_val;
        hw->latest_pending = 0;
        hw->sending_latest = 1;
    } else {
        hw->out_cmd = hw->cmd[hw->head];
        hw->out_val = hw->val[hw->head];
        hw->sending_latest = 0;
    }
    hw->last_latest = hw->sending_latest;
    hw->attempts = 0;
    return transmit(hw, now);
}

/* The outstanding frame is done with, ACKed or abandoned. After a single
 * copy the next frame follows at once (CHWAKE never dropped, so the host is
 * still awake); after a resend, quarantine first. `since` still holds the
 * time the last copy went out, which is what the quarantine runs from. */
static host_wake_step_t retire(host_wake_t *hw, uint32_t now, uint8_t abandoned)
{
    host_wake_step_t out = {1, 0, 0, 0};
    uint8_t resent = (uint8_t)(hw->attempts > 1u || abandoned);

    if (!hw->sending_latest) {
        hw->head = (uint8_t)((hw->head + 1u) % HOST_WAKE_QUEUE_SIZE);
        hw->count--;
    } else if (abandoned && !hw->latest_pending) {
        hw->latest_lost = 1;
    }
    hw->sending_latest = 0;
    hw->retired++;
    if (abandoned) {
        hw->abandoned++;
    }
    hw->attempts = 0;
    hw->acked = 0;

    if (resent) {
        hw->state = HOST_WAKE_STATE_QUARANTINE;
        return out;
    }
    if (have_pending(hw)) {
        return send_next(hw, now);
    }
    hw->state = HOST_WAKE_STATE_IDLE;
    out.line = 0;
    return out;
}

host_wake_step_t HostWake_Step(host_wake_t *hw, uint32_t now)
{
    host_wake_step_t out = {1, 0, 0, 0};

    switch (hw->state) {
    case HOST_WAKE_STATE_GUARD:
        if ((uint32_t)(now - hw->since) < hw->guard_ticks) {
            return out;
        }
        return send_next(hw, now);

    case HOST_WAKE_STATE_WAIT_ACK:
        if (hw->acked) {
            return retire(hw, now, 0);
        }
        if ((uint32_t)(now - hw->since) < hw->ack_timeout_ticks) {
            return out;
        }
        if (hw->attempts >= hw->max_attempts) {
            return retire(hw, now, 1);
        }
        return transmit(hw, now);

    case HOST_WAKE_STATE_QUARANTINE:
        /* Until two ACK timeouts after the last copy went out. CHWAKE stays
         * high: the host stays awake for what follows. */
        if ((uint32_t)(now - hw->since) < 2u * hw->ack_timeout_ticks) {
            return out;
        }
        if (have_pending(hw)) {
            return send_next(hw, now);
        }
        hw->state = HOST_WAKE_STATE_IDLE;
        out.line = 0;
        return out;

    default: /* HOST_WAKE_STATE_IDLE */
        if (!have_pending(hw)) {
            out.line = 0;
            return out;
        }
        hw->state = HOST_WAKE_STATE_GUARD;
        hw->since = now;
        return out;
    }
}

uint8_t HostWake_Idle(const host_wake_t *hw)
{
    return hw->state == HOST_WAKE_STATE_IDLE && !have_pending(hw);
}
