/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 *
 * The application image identity header instance. Placed by the linker at
 * ORIGIN(FLASH) + 0x20 in the .kbd_id section, which ld/ch592f.ld pins and
 * asserts. `used` keeps it despite -fdata-sections/--gc-sections and it
 * having no C references (hosts read it out of the .bin/flash, not through
 * this symbol). base is the slot this build links for, so a host can refuse
 * an image whose base disagrees with the device's write slot; family 0xB2
 * lets shared bench tooling refuse dongle images outright.
 */
#include "kbd_image_id.h"

#ifndef OPENBOOT_SLOT_BASE
#error "OPENBOOT_SLOT_BASE must be defined (see firmware/Makefile)"
#endif

/* Linker-provided; only its ADDRESS carries the value (loadable image size). */
extern const char _kbd_image_len[];

const kbd_image_id_t
__attribute__((section(".kbd_id"), used, aligned(4)))
kbd_image_id = {
    { KBD_IMAGE_ID_MAGIC0, KBD_IMAGE_ID_MAGIC1,
      KBD_IMAGE_ID_MAGIC2, KBD_IMAGE_ID_MAGIC3 },
    KBD_IMAGE_ID_FORMAT,
    KBD_CHIP_FAMILY_ID,
    KBD_IMAGE_KIND_APP,
    KBD_IMAGE_ID_LEN,
    OPENBOOT_SLOT_BASE,
    (uint32_t)(uintptr_t)_kbd_image_len,
    0u,
    0u,
    0u,
    0u,
};
