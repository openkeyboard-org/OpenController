/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * OpenController keyboard wireless-module firmware linker script (CH592F).
 * Based on CH592 SDK Link.ld
 *
 * The application runs under OpenBoot (third_party/openboot), which owns
 * flash [0x0000, 0x2000) and boots one of two equal A/B slots:
 *
 *   slot A image 0x02000..0x37FFF   OBR2 boot record 0x38000..0x38FFF
 *   slot B image 0x39000..0x6EFFF   OBR2 boot record 0x6F000..0x6FFFF
 *
 * The MEMORY block is generated per slot into $(BUILD)/memory.ld (see
 * firmware/Makefile) so this file holds no literal addresses. FLASH LENGTH
 * is the slot CAPACITY (slot size minus the 4 KB record block): an image
 * linked into the record block would boot once and then be destroyed by the
 * first COMMIT, which erases that block to write the new record.
 *
 * RAM LENGTH is 0x67F0, not 26K: OpenBoot's boot-request mailbox occupies
 * the top 16 bytes of SRAM (0x200067F0), written by openboot_request_update()
 * and consumed by the bootloader after the soft reset. See firmware/BOOT.md.
 */

INCLUDE memory.ld

ENTRY( _start )

__stack_size = 2048;
PROVIDE( _stack_size = __stack_size );

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

    /* 32-byte ODG2-format identity header (family 0xB2 = OpenController
     * keyboard CH592) at image offset 0x20, host-readable from the .bin
     * without symbols. See src/kbd_image_id.c. */
    /* No AT> here: the LMA must follow the VMA jump to ORIGIN+0x20, or the
     * header's load address stays glued right after .init and the bytes land
     * at file offset 0x04 in the .bin. */
    .kbd_id ORIGIN(FLASH) + 0x20 :
    {
        _kbd_id_start = .;
        KEEP(*(.kbd_id))
        _kbd_id_end = .;
    } >FLASH

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

    ASSERT(_ebss <= _diag_scratch_start, "BSS collides with the diag scratch window")

    PROVIDE( _end = _ebss);
    PROVIDE( end = . );

    /* Fixed-address diagnostic window, pinned ABSOLUTELY so bench SWD
     * addresses never move with the memory map:
     *   [0x20005800, 0x20005820)  asm-owned scratch: boot phase sentinel
     *       (startup_CH592_phased.S) and fault latches (fault_handler.S) -
     *       those files hard-code these addresses.
     *   [0x20005820, 0x20005A00)  .diag_safe counters (rf_task/main),
     *       readable via SWD without symbol lookup.
     * NOTE: pre-OpenBoot builds placed .diag_safe at 0x20005E00; bench
     * scripts reading counters by absolute address must re-derive them. */
    _diag_scratch_start = 0x20005800;
    _diag_scratch_end   = 0x20005820;

    .diag_safe 0x20005820 (NOLOAD) :
    {
        . = ALIGN(4);
        PROVIDE( _diag_safe_start = . );
        *(.diag_safe)
        *(.diag_safe.*)
        . = ALIGN(4);
        PROVIDE( _diag_safe_end = . );
    } >RAM

    ASSERT(_diag_safe_start == _diag_scratch_end,
           "diag counters must start right after the asm scratch window")
    ASSERT(_diag_safe_end <= _diag_scratch_start + 0x200,
           ".diag_safe overflows the 0x200 diag window")

    .stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size :
    {
        PROVIDE( _heap_end = . );
        . = ALIGN(4);
        PROVIDE(_susrstack = . );
        . = . + __stack_size;
        PROVIDE( _eusrstack = .);
    } >RAM

    ASSERT(_diag_safe_end <= _susrstack, ".diag_safe overflows into the stack region")
    ASSERT(_eusrstack == OC_BOOTREQ_ADDR,
           "stack top must sit just below the OpenBoot boot-request word (memory.ld)")
    ASSERT(ORIGIN(FLASH) == OC_SLOT_A_BASE || ORIGIN(FLASH) == OC_SLOT_B_BASE,
           "app must link at one of OpenBoot's slot bases (see memory.ld)")
    ASSERT(_kbd_id_start == ORIGIN(FLASH) + 0x20,
           "identity header must sit at image offset 0x20")
    ASSERT(LOADADDR(.kbd_id) == ORIGIN(FLASH) + 0x20,
           "identity header LMA must equal its VMA - the .bin offset follows the")
    /* ^ load address, not the VMA; this script mixes AT>FLASH and non-AT
     * sections, and an LMA regression here was hit once during bring-up
     * (header bytes landed at file offset 0x04). */
    ASSERT(_kbd_id_end - _kbd_id_start == 0x20,
           "identity header must be exactly 32 bytes")
    ASSERT(_einit <= _kbd_id_start, ".init overruns the identity header")

    /* Image length for the identity header: end of the last loadable flash
     * byte (the .data initializers), relative to the slot base. */
    _kbd_image_len = ((LOADADDR(.data) + SIZEOF(.data) + 3) & ~3) - ORIGIN(FLASH);
}
