/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * RAM-resident code for a build with the BLE mode. Included by
 * ld/ch592f.ld.
 *
 * A boot runs either the 2.4 GHz link or BLE, never both
 * (transport_mode.h), so the RAM code only one of them uses shares one
 * window: .highcode_2g4 (rf_task.c) and .highcode_ble (the BLE library's
 * connection, channel-hop and address-list objects, none of which a
 * 2.4 GHz-only build links at all) both run at the same address, and
 * main() copies the booted mode's image there before starting either
 * stack. Everything else is copied by the startup code as before.
 *
 * WCH's CH59x_pwr.c puts all four of its sleep routines in RAM, and only
 * LowPower_Sleep is used. The Makefile builds that file with __HIGH_CODE
 * empty, so each routine gets its own section, the unused ones are
 * collected as garbage, and LowPower_Sleep is pulled in here by name. */

    .highcode :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_vma_start = .);
        *(.vector);
        KEEP(*(SORT_NONE(.vector_handler)))
        EXCLUDE_FILE(*rf_task.o
                     *LIBCH59xBLE.a:ll_connect.o
                     *LIBCH59xBLE.a:ll_hop.o
                     *LIBCH59xBLE.a:ll_resolvinglist.o
                     *LIBCH59xBLE.a:ll_whitelist.o) *(.highcode .highcode.*)
        *CH59x_pwr.o(.text.LowPower_Sleep)
        . = ALIGN(4);
        PROVIDE(_highcode_vma_end = .);
    } >RAM AT>FLASH

    OVERLAY ADDR(.highcode) + SIZEOF(.highcode) : NOCROSSREFS
            AT (LOADADDR(.highcode) + SIZEOF(.highcode))
    {
        .highcode_2g4
        {
            *rf_task.o(.highcode .highcode.*)
            . = ALIGN(4);
        }
        .highcode_ble
        {
            *LIBCH59xBLE.a:ll_connect.o(.highcode .highcode.*)
            *LIBCH59xBLE.a:ll_hop.o(.highcode .highcode.*)
            *LIBCH59xBLE.a:ll_resolvinglist.o(.highcode .highcode.*)
            *LIBCH59xBLE.a:ll_whitelist.o(.highcode .highcode.*)
            . = ALIGN(4);
        }
    } >RAM

    PROVIDE(_highcode_overlay_vma = ADDR(.highcode_2g4));
    _highcode_ram_end = ADDR(.highcode_2g4)
                        + MAX(SIZEOF(.highcode_2g4), SIZEOF(.highcode_ble));
    _text_start = LOADADDR(.highcode_ble) + SIZEOF(.highcode_ble);
