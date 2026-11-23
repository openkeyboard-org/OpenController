/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE HID keyboard (HID over GATT), the module's Bluetooth mode.
 *
 * Runs only on a boot whose transport record selects BLE (transport_mode.h):
 * the 2.4 GHz task is never started on such a boot. One profile per boot;
 * each profile has its own bond block, static address and IRK. All entry
 * points are main-loop context.
 *
 * Statuses sent to the keyboard host (5B xx), as the vendor protocol uses
 * them: 31 open for pairing, 32 connected (encrypted), 33 disconnected,
 * 34 transport selected, 35 reconnecting, 36 no bond or pairing rejected.
 */
#ifndef BLE_HID_H
#define BLE_HID_H

#include <stdint.h>

/* After HAL_Init on a BLE boot: GAP peripheral role, GATT services, the
 * app task. The boot statuses (34, then 35 or 36) follow once the stack
 * has started. */
void BleHid_Init(uint8_t profile, uint8_t generation);

/* A6 3n for this boot's profile. Idempotent: resumes the reconnect after an
 * A6 11 and otherwise only reports where the link stands. */
void BleHid_Select(void);

/* A6 11: drop the link and stop advertising (the keyboard is on USB). */
void BleHid_Idle(void);

/* A6 51: erase this profile's bond and open it to pairing, under a new
 * address. Only from where a first pairing starts (no bond, no link, a
 * blank bond block); otherwise the caller restarts the module first
 * (BleHid_PairNeedsRestart) and the new boot pairs once its stack has
 * started (BleHid_PairAtStart).
 * Asked again while the pairing window is open, it only restarts the
 * window. */
void BleHid_Pair(void);
uint8_t BleHid_PairNeedsRestart(void);
void BleHid_PairAtStart(void);

/* A6 52: erase this profile's bond. */
void BleHid_Unpair(void);

/* A1: one boot keyboard report, delivered in order once the link is up. */
void BleHid_QueueReport(const uint8_t report[8]);

/* A3: a consumer (media) usage, 0 for none; A4: the vendor system bitmap
 * (bit 0 Power Down, 1 Sleep, 2 Wake Up), which goes to the host as it
 * is. Queued in order with the keyboard reports. */
void BleHid_QueueConsumer(uint16_t usage);
void BleHid_QueueSystem(uint8_t bits);

/* A9: device name; a '$' becomes the profile number (1-3). The caller
 * also stores it (TransportMode_StoreName) for later boots. */
void BleHid_SetName(const uint8_t *name, uint8_t len);

/* A7: the battery level, 0-100 %, for the Battery Service. */
void BleHid_SetBattery(uint8_t percent);

/* Main loop: deliver queued reports. */
void BleHid_Service(void);

/* Before a reset or OpenBoot: end the link and stop advertising. */
void BleHid_Quiesce(void);

/* Nonzero when nothing is connected or advertising. */
uint8_t BleHid_Quiet(void);

/* Nonzero when the module may sleep between radio events (KBD_BLE_SLEEP):
 * nothing queued, no pair, unpair or switch under way, and the rest
 * hold-off after the last keyboard report has passed. */
uint8_t BleHid_CanSleep(void);

/* The LED state sent to the keyboard host was abandoned unacknowledged
 * (host wake): offer it again. */
void BleHid_LedResync(void);

#endif
