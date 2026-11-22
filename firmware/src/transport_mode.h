/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which radio stack this boot runs, and the selected Bluetooth profile.
 *
 * The 2.4 GHz link and BLE share the CH592's radio and can't run together:
 * the 2.4 GHz link drives the radio in basic mode from main-loop polling
 * and its own TMR0/RTC timing, which TMOS can't see. So the mode is chosen
 * once, at boot, from a record in DataFlash, and a change of mode (or of
 * BLE profile) writes the record and resets the module. WCH's own tri-mode
 * mouse switches the same way.
 *
 * DataFlash map (offsets from 0x70000, 256-byte erase pages):
 *   0x4000  2.4 GHz bond (rf_task.c)
 *   0x6000  stock vendor flags (left alone)
 *   0x7B00  this record (mode, profile, generations, BLE name)
 *   0x7C00  BLE bonds, profile 1 (one 256-byte SNV block per profile)
 *   0x7D00  BLE bonds, profile 2
 *   0x7E00  BLE bonds, profile 3
 */
#ifndef TRANSPORT_MODE_H
#define TRANSPORT_MODE_H

#include <stdint.h>

#define TRANSPORT_MODE_2G4      0u
#define TRANSPORT_MODE_BLE      1u

#define TRANSPORT_BLE_PROFILES  3u
/* The A9 frame carries at most 18 name bytes. */
#define TRANSPORT_NAME_MAX      18u

#define TRANSPORT_RECORD_OFF    0x7B00u
#define TRANSPORT_SNV_BASE_OFF  0x7C00u
#define TRANSPORT_SNV_BLOCK     0x100u
#define TRANSPORT_SNV_OFF(profile) \
    (TRANSPORT_SNV_BASE_OFF + TRANSPORT_SNV_BLOCK * (uint32_t)(profile))

/* Read the record. On a module without one (first boot of this firmware,
 * or a module that ran other firmware) the BLE bond blocks are erased, so
 * nothing left there is taken for a bond, and 2.4 GHz is selected. Call
 * before CH59x_BLEInit: the BLE library reads its bonds at init. */
void TransportMode_Load(void);

uint8_t TransportMode_IsBle(void);

/* 0-based BLE profile this boot runs (0 in 2.4 GHz mode as well). */
uint8_t TransportMode_Profile(void);

/* Bumped every time a profile is paired afresh; mixed into the profile's
 * static address so a host holding a stale bond sees a new device. */
uint8_t TransportMode_Generation(uint8_t profile);

/* Nonzero when (mode, profile) differs from what this boot runs: the
 * caller must then store it and reset. */
uint8_t TransportMode_NeedsReset(uint8_t mode, uint8_t profile);

/* Write the record for the next boot, marked so that boot answers the
 * select that caused it. Main-loop context only (DataFlash). */
void TransportMode_Store(uint8_t mode, uint8_t profile);

/* Nonzero on the boot that follows a switch: it must report 5B 34 and the
 * link state even where an ordinary boot stays silent. */
uint8_t TransportMode_BootAnnounces(void);

/* Bump `profile`'s generation and write the record. Main-loop context. */
void TransportMode_BumpGeneration(uint8_t profile);

/* The BLE device name the keyboard host last sent (A9), kept so a mode or
 * profile switch, which resets the module, doesn't lose it. A length of 0
 * means none was ever sent. */
const uint8_t *TransportMode_Name(uint8_t *len);

/* Store a name from A9; writes DataFlash only when it changed. Main-loop
 * context. */
void TransportMode_StoreName(const uint8_t *name, uint8_t len);

/* Software reset into the stored mode. OpenBoot boots the app straight
 * away when no update was requested. */
void TransportMode_Reset(void) __attribute__((noreturn));

#endif
