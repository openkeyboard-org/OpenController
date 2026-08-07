/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 *
 * OpenController keyboard wireless-module firmware linker script (CH592F).
 * Based on CH592 SDK Link.ld
 *
 * Flash base 0x1000 preserves the 4 KB vendor stage-1 TRAMPOLINE
 * at 0x0000-0x0FFF (which jumps to real stage-1 at 0x6D000).
 *
 * FLASH length is capped at 216 KB (ends at 0x36FFF). Anything above
 * that overlaps the vendor's flash layout and is destructive:
 *   0x37000 - 0x6CFFF : IAP staging region. Wiped on every IAP flash;
 *                       app code placed here disappears silently.
 *   0x6D000 - 0x6FFFF : Vendor stage-1 code. Overlapping this BRICKS
 *                       the boot path - the chip cannot boot and
 *                       recovery needs SWD + wchisp via BOOT pin.
 * Keeping the limit at 0x36FFF lets the replacement firmware coexist
 * with the stock stage-1 + IAP flow. See the flash-map section of
 * ../README.md for details.
 */

ENTRY( _start )

__stack_size = 2048;
PROVIDE( _stack_size = __stack_size );

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00001000, LENGTH = 216K
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 26K
}

SECTIONS
{
    .init :
    {
        _sinit = .;
        . = ALIGN(4);
        KEEP(*(SORT_NONE(.init)))
        . = ALIGN(4);
        _einit = .;
    } >FLASH AT>FLASH

    .highcodelalign :
    {
        . = ALIGN(4);
        PROVIDE(_highcode_lma = .);
    } >FLASH AT>FLASH

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

    .text :
    {
        . = ALIGN(4);
        KEEP(*(SORT_NONE(.handle_reset)))
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata*)
        *(.sdata2.*)
        *(.glue_7)
        *(.glue_7t)
        *(.gnu.linkonce.t.*)
        . = ALIGN(4);
    } >FLASH AT>FLASH

    .fini :
    {
        KEEP(*(SORT_NONE(.fini)))
        . = ALIGN(4);
    } >FLASH AT>FLASH

    PROVIDE( _etext = . );
    PROVIDE( _eitcm = . );

    .preinit_array :
    {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } >FLASH AT>FLASH

    .init_array :
    {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
        PROVIDE_HIDDEN (__init_array_end = .);
    } >FLASH AT>FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } >FLASH AT>FLASH

    .ctors :
    {
        KEEP (*crtbegin.o(.ctors))
        KEEP (*crtbegin?.o(.ctors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
        KEEP (*(SORT(.ctors.*)))
        KEEP (*(.ctors))
    } >FLASH AT>FLASH

    .dtors :
    {
        KEEP (*crtbegin.o(.dtors))
        KEEP (*crtbegin?.o(.dtors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
        KEEP (*(SORT(.dtors.*)))
        KEEP (*(.dtors))
    } >FLASH AT>FLASH

    .dalign :
    {
        . = ORIGIN(RAM) + SIZEOF(.highcode);
    } >RAM AT>FLASH

    .dlalign :
    {
        . = ALIGN(4);
        PROVIDE(_data_lma = .);
    } >FLASH AT>FLASH

    .data :
    {
        . = ALIGN(4);
        PROVIDE(_data_vma = .);
        *(.gnu.linkonce.r.*)
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
        PROVIDE( __global_pointer$ = . + 0x800 );
        *(.sdata .sdata.*)
        *(.gnu.linkonce.s.*)
        . = ALIGN(8);
        *(.srodata.cst16)
        *(.srodata.cst8)
        *(.srodata.cst4)
        *(.srodata.cst2)
        *(.srodata .srodata.*)
        . = ALIGN(4);
        PROVIDE( _edata = .);
    } >RAM AT>FLASH

    .bss :
    {
        . = ALIGN(4);
        PROVIDE( _sbss = .);
        *(.sbss*)
        *(.gnu.linkonce.sb.*)
        *(.bss*)
        *(.gnu.linkonce.b.*)
        *(COMMON*)
        . = ALIGN(4);
        PROVIDE( _ebss = .);
    } >RAM AT>FLASH

    ASSERT(_ebss <= 0x20005800, "BSS collides with diag scratch")

    PROVIDE( _end = _ebss);
    PROVIDE( end = . );

    /* Fixed-address diagnostic counters. Placed in the SRAM gap between
     * _ebss and the stack so they survive both BLE-library BSS/MEM_BUF
     * overruns (below) and ISR stack growth (above). Pinned at
     * SRAM_END - stack_size - 0x200 so the address is stable across
     * rebuilds; any rf_task / main symbol in section ".diag_safe" lands
     * here, readable directly via SWD without symbol lookup. */
    .diag_safe ORIGIN(RAM) + LENGTH(RAM) - __stack_size - 0x200 (NOLOAD) :
    {
        . = ALIGN(4);
        PROVIDE( _diag_safe_start = . );
        *(.diag_safe)
        *(.diag_safe.*)
        . = ALIGN(4);
        PROVIDE( _diag_safe_end = . );
    } >RAM

    ASSERT(_diag_safe_end <= ORIGIN(RAM) + LENGTH(RAM) - __stack_size,
           ".diag_safe overflows into the stack region")

    .stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size :
    {
        PROVIDE( _heap_end = . );
        . = ALIGN(4);
        PROVIDE(_susrstack = . );
        . = . + __stack_size;
        PROVIDE( _eusrstack = .);
    } >RAM
}
