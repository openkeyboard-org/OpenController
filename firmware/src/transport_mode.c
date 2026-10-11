/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Boot-time transport record; see transport_mode.h.
 */

#include "CONFIG.h"
#include "transport_mode.h"

#define TRANSPORT_RECORD_MAGIC    0x4D54434Fu   /* "OCTM" */
#define TRANSPORT_RECORD_VERSION  1u
#define TRANSPORT_ERASE_LEN       256u

/* Set by a switch, cleared by the boot it caused: that boot owes the host
 * the select's answer (5B 34 and the link state). */
#define TRANSPORT_FLAG_ANNOUNCE   0x01u

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  mode;
    uint8_t  profile;
    uint8_t  generation[TRANSPORT_BLE_PROFILES];
    uint8_t  flags;
    uint8_t  check;
} transport_record_t;

_Static_assert(sizeof(transport_record_t) == 12, "record layout");

/* `current` is what DataFlash holds; `booted_*` is what this boot runs. They
 * differ between a Store and the reset that follows it. */
static transport_record_t current __attribute__((aligned(4)));
static uint8_t booted_mode;
static uint8_t booted_profile;
static uint8_t booted_announce;

static uint8_t record_check(const transport_record_t *rec)
{
    const uint8_t *p = (const uint8_t *)rec;
    uint8_t sum = 0xA5u;

    for (uint8_t i = 0; i < sizeof(*rec) - 1u; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum;
}

static uint8_t record_valid(const transport_record_t *rec)
{
    return rec->magic == TRANSPORT_RECORD_MAGIC
        && rec->version == TRANSPORT_RECORD_VERSION
        && rec->mode <= TRANSPORT_MODE_BLE
        && rec->profile < TRANSPORT_BLE_PROFILES
        && rec->check == record_check(rec);
}

static void record_write(void)
{
    current.check = record_check(&current);
    if (EEPROM_ERASE(TRANSPORT_RECORD_OFF, TRANSPORT_ERASE_LEN) != 0) {
        return;
    }
    (void)EEPROM_WRITE(TRANSPORT_RECORD_OFF, &current, sizeof(current));
}

void TransportMode_Load(void)
{
    if (EEPROM_READ(TRANSPORT_RECORD_OFF, &current, sizeof(current)) == 0
            && record_valid(&current)) {
        booted_mode = current.mode;
        booted_profile = current.profile;
        if (current.flags & TRANSPORT_FLAG_ANNOUNCE) {
            booted_announce = 1;
            current.flags &= (uint8_t)~TRANSPORT_FLAG_ANNOUNCE;
            record_write();
        }
        return;
    }

    /* No record: whatever the BLE bond blocks hold was not written by this
     * firmware (a module shipped with vendor firmware keeps its own data up
     * here), so it must not be loaded as a bond. */
    for (uint8_t p = 0; p < TRANSPORT_BLE_PROFILES; p++) {
        (void)EEPROM_ERASE(TRANSPORT_SNV_OFF(p), TRANSPORT_ERASE_LEN);
    }
    tmos_memset(&current, 0, sizeof(current));
    current.magic = TRANSPORT_RECORD_MAGIC;
    current.version = TRANSPORT_RECORD_VERSION;
    current.mode = TRANSPORT_MODE_2G4;
    record_write();
    booted_mode = TRANSPORT_MODE_2G4;
    booted_profile = 0;
}

uint8_t TransportMode_IsBle(void)
{
    return booted_mode == TRANSPORT_MODE_BLE;
}

uint8_t TransportMode_Profile(void)
{
    return booted_profile;
}

uint8_t TransportMode_Generation(uint8_t profile)
{
    return profile < TRANSPORT_BLE_PROFILES ? current.generation[profile] : 0u;
}

uint8_t TransportMode_NeedsReset(uint8_t mode, uint8_t profile)
{
    if (mode != booted_mode) {
        return 1;
    }
    return mode == TRANSPORT_MODE_BLE && profile != booted_profile;
}

void TransportMode_Store(uint8_t mode, uint8_t profile)
{
    if (mode > TRANSPORT_MODE_BLE || profile >= TRANSPORT_BLE_PROFILES) {
        return;
    }
    if (mode == TRANSPORT_MODE_2G4) {
        profile = current.profile;   /* keep the BLE profile for next time */
    }
    if (mode == current.mode && profile == current.profile) {
        return;
    }
    current.mode = mode;
    current.profile = profile;
    current.flags |= TRANSPORT_FLAG_ANNOUNCE;
    record_write();
}

uint8_t TransportMode_BootAnnounces(void)
{
    return booted_announce;
}

void TransportMode_BumpGeneration(uint8_t profile)
{
    if (profile >= TRANSPORT_BLE_PROFILES) {
        return;
    }
    current.generation[profile]++;
    record_write();
}

void TransportMode_Reset(void)
{
    /* The safe-access window closes ~16 system cycles after SIG2, so the
     * protected write follows at once (as openboot_request_update does). */
    R8_SAFE_ACCESS_SIG = SAFE_ACCESS_SIG1;
    R8_SAFE_ACCESS_SIG = SAFE_ACCESS_SIG2;
    __asm__ volatile ("nop\n\tnop" ::: "memory");
    R8_RST_WDOG_CTRL |= RB_SOFTWARE_RESET;
    R8_SAFE_ACCESS_SIG = SAFE_ACCESS_SIG0;
    for (;;) {
    }
}
