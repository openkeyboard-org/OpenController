/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * RAM-resident code for a build without the BLE mode: one section, copied
 * to RAM by the startup code. Included by ld/ch592f.ld. */

    .highcode :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_vma_start = .);
        *(.vector);
        KEEP(*(SORT_NONE(.vector_handler)))
        *(.highcode);
        *(.highcode.*);
        . = ALIGN(4);
        PROVIDE(_highcode_vma_end = .);
    } >RAM AT>FLASH

    _highcode_ram_end = ADDR(.highcode) + SIZEOF(.highcode);
    _text_start = LOADADDR(.highcode) + SIZEOF(.highcode);
