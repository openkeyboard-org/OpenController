/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 *
 * OpenController application image identity header. Wire-compatible with
 * OpenDongle's ODG2 format so the same host-side parsers work; the family
 * code distinguishes keyboard images from dongle images (both are CH592
 * binaries that OpenBoot itself cannot tell apart - COMMIT attests only
 * length + CRC).
 */
#ifndef KBD_IMAGE_ID_H
#define KBD_IMAGE_ID_H

#include <stdint.h>

#define KBD_IMAGE_ID_OFF     0x20u
#define KBD_IMAGE_ID_LEN     0x20u

#define KBD_IMAGE_ID_MAGIC0  'O'
#define KBD_IMAGE_ID_MAGIC1  'D'
#define KBD_IMAGE_ID_MAGIC2  'G'
#define KBD_IMAGE_ID_MAGIC3  '2'
#define KBD_IMAGE_ID_FORMAT  2u

/* Keyboard-side CH592 wireless module (dongle families: 0x92 CH592,
 * 0x70 CH570). */
#define KBD_CHIP_FAMILY_ID   0xB2u

#define KBD_IMAGE_KIND_APP   0x01u

typedef struct {
    uint8_t  magic[4];
    uint8_t  format_ver;
    uint8_t  family;
    uint8_t  image_kind;
    uint8_t  header_len;
    uint32_t base;
    uint32_t image_len;
    uint32_t image_crc32;   /* 0 in v1: no post-link finalize step */
    uint32_t build_id;      /* 0 in v1 */
    uint32_t flags;
    uint32_t extension_len;
} kbd_image_id_t;

_Static_assert(sizeof(kbd_image_id_t) == KBD_IMAGE_ID_LEN,
               "ODG2 header must be exactly 32 bytes");

extern const kbd_image_id_t kbd_image_id;

#endif /* KBD_IMAGE_ID_H */
