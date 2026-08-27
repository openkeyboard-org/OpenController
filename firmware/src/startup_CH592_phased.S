/*
 * Copyright 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phased CH592F startup — fork of WCH's vendor startup_CH592.S with SRAM
 * markers at each reset phase. The vendor logic is Copyright (c) Nanjing
 * Qinheng Microelectronics Co., Ltd. and remains under WCH's notice (see
 * ../NOTICE); only the marker stores are additions.
 *
 * Sentinel byte at 0x20005800 advances
 * through 0xC0 .. 0xC5 as reset progresses, then main() writes 0xA0+.
 * Whichever value persists tells us exactly where the wedge happens
 * without depending on GDB attach/halt behavior.
 *
 * Marker map:
 *   0xC0 — first instruction in reset handler (no stack, no gp yet)
 *   0xC1 — after gp + sp setup
 *   0xC2 — after .highcode flash→SRAM copy
 *   0xC3 — after .data flash→SRAM copy
 *   0xC4 — after .bss zero-clear
 *   0xC5 — after CSR/mtvec setup, immediately before mret to main
 *   0xA0+ — main() entry sentinel writes
 *
 * Vendor startup is otherwise byte-for-byte identical to the CH592 SDK's
 * SRC/Startup/startup_CH592.S (no logic changes — only marker stores added).
 */

	.section	.init,"ax",@progbits
	.global	_start
	.align	1
_start:
	j	handle_reset

    .section    .vector,"ax",@progbits
    .align  1
_vector_base:
    .option norvc;

    .word   0
    .word   0
    .word   NMI_Handler                 /* NMI Handler */
    .word   HardFault_Handler           /* Hard Fault Handler */
    .word   0xF5F9BDA9
    .word   Ecall_M_Mode_Handler        /* 5 */
    .word   0
    .word   0
    .word   Ecall_U_Mode_Handler		/* 8 */
    .word   Break_Point_Handler			/* 9 */
    .word   0
    .word   0
    .word   SysTick_Handler            /* SysTick Handler */
    .word   0
    .word   SW_Handler                 /* SW Handler */
    .word   0
    /* External Interrupts */
    .word   TMR0_IRQHandler            /* 0:  TMR0 */
    .word   GPIOA_IRQHandler           /* GPIOA */
    .word   GPIOB_IRQHandler           /* GPIOB */
    .word   SPI0_IRQHandler            /* SPI0 */
    .word   BB_IRQHandler              /* BLEB */
    .word   LLE_IRQHandler             /* BLEL */
    .word   USB_IRQHandler             /* USB */
    .word   0
    .word   TMR1_IRQHandler            /* TMR1 */
    .word   TMR2_IRQHandler            /* TMR2 */
    .word   UART0_IRQHandler           /* UART0 */
    .word   UART1_IRQHandler           /* UART1 */
    .word   RTC_IRQHandler             /* RTC */
    .word   ADC_IRQHandler             /* ADC */
    .word   I2C_IRQHandler 			   /* I2C */
    .word   PWMX_IRQHandler            /* PWMX */
    .word   TMR3_IRQHandler            /* TMR3 */
    .word   UART2_IRQHandler           /* UART2 */
    .word   UART3_IRQHandler           /* UART3 */
    .word   WDOG_BAT_IRQHandler        /* WDOG_BAT */

    .option rvc;

    .section    .vector_handler, "ax", @progbits
    .weak   NMI_Handler
    .weak   HardFault_Handler
    .weak   Ecall_M_Mode_Handler
    .weak   Ecall_U_Mode_Handler
    .weak   Break_Point_Handler
    .weak   SysTick_Handler
    .weak   SW_Handler
    .weak   TMR0_IRQHandler
    .weak   GPIOA_IRQHandler
    .weak   GPIOB_IRQHandler
    .weak   SPI0_IRQHandler
    .weak   BB_IRQHandler
    .weak   LLE_IRQHandler
    .weak   USB_IRQHandler
    .weak   TMR1_IRQHandler
    .weak   TMR2_IRQHandler
    .weak   UART0_IRQHandler
    .weak   UART1_IRQHandler
    .weak   RTC_IRQHandler
    .weak   ADC_IRQHandler
    .weak   I2C_IRQHandler
    .weak   PWMX_IRQHandler
    .weak   TMR3_IRQHandler
    .weak   UART2_IRQHandler
    .weak   UART3_IRQHandler
    .weak   WDOG_BAT_IRQHandler

NMI_Handler:
HardFault_Handler:
Ecall_M_Mode_Handler:
Ecall_U_Mode_Handler:
Break_Point_Handler:
SysTick_Handler:
SW_Handler:
TMR0_IRQHandler:
GPIOA_IRQHandler:
GPIOB_IRQHandler:
SPI0_IRQHandler:
BB_IRQHandler:
LLE_IRQHandler:
USB_IRQHandler:
TMR1_IRQHandler:
TMR2_IRQHandler:
UART0_IRQHandler:
UART1_IRQHandler:
RTC_IRQHandler:
ADC_IRQHandler:
I2C_IRQHandler:
PWMX_IRQHandler:
TMR3_IRQHandler:
UART2_IRQHandler:
UART3_IRQHandler:
WDOG_BAT_IRQHandler:
1:
	j 1b

	.section	.handle_reset,"ax",@progbits
	.weak	handle_reset
	.align	1
handle_reset:
	/* PHASE 0xC0 — first instruction. No gp/sp yet, so use direct
	 * SRAM address (0x20005800). Manual LUI+SB sequence; no temporary
	 * gp-relative addressing.
	 *
	 * Capture R8_RESET_STATUS @ 0x40001044 BEFORE any other init so the
	 * reset cause (SW / RPOR / WTR / MR / LRM[0-2] / GPWSM per CH59x
	 * SYS_ResetStaTypeDef) is preserved across BSS clear. Stashed to
	 * 0x20005801 (one byte; sits next to boot_sentinel). The fault
	 * handler in src/fault_handler.S writes 0x20005804.. on faults.
	 */
	li t1, 0x40001044
	lb t0, 0(t1)
	li t1, 0x20005801
	sb t0, 0(t1)

	li t0, 0xC0
	li t1, 0x20005800
	sb t0, 0(t1)

.option push
.option	norelax
	la gp, __global_pointer$
.option	pop
1:
	la sp, _eusrstack

	/* PHASE 0xC1 — after gp + sp setup. */
	li t0, 0xC1
	li t1, 0x20005800
	sb t0, 0(t1)

/* Load highcode code  section from flash to RAM */
2:
    la a0, _highcode_lma
    la a1, _highcode_vma_start
    la a2, _highcode_vma_end
    bgeu a1, a2, 2f
1:
    lw t0, (a0)
    sw t0, (a1)
    addi a0, a0, 4
    addi a1, a1, 4
    bltu a1, a2, 1b

	/* PHASE 0xC2 — after .highcode copy. */
	li t0, 0xC2
	li t1, 0x20005800
	sb t0, 0(t1)

/* Load data section from flash to RAM */
2:
	la a0, _data_lma
	la a1, _data_vma
	la a2, _edata
	bgeu a1, a2, 2f
1:
	lw t0, (a0)
	sw t0, (a1)
	addi a0, a0, 4
	addi a1, a1, 4
	bltu a1, a2, 1b
2:
	/* PHASE 0xC3 — after .data copy. */
	li t0, 0xC3
	li t1, 0x20005800
	sb t0, 0(t1)

	/* clear bss section */
	la a0, _sbss
	la a1, _ebss
	bgeu a0, a1, 2f
1:
	sw zero, (a0)
	addi a0, a0, 4
	bltu a0, a1, 1b
2:
	/* PHASE 0xC4 — after .bss zero-clear. NOTE: bss-clear would zero
	 * any in-bss sentinel; ours at 0x20005800 is in the gap (above
	 * _ebss=0x20003718, below stack start 0x20006000) so it survives. */
	li t0, 0xC4
	li t1, 0x20005800
	sb t0, 0(t1)

	/* pipeline + dynamic prediction reset */
	li t0, 0x1f
	csrw 0xbc0, t0
	/* nested interrupts + hardware stacking enable */
	li t0, 0x3
	csrw 0x804, t0

    li t0, 0x88
    csrw mstatus, t0
	la t0, _vector_base

	/* mtvec = vector base + absolute-address mode */
    ori t0, t0, 3
	csrw mtvec, t0

	la t0, main
	csrw mepc, t0

	/* PHASE 0xC5 — immediately before mret to main. If main is reached,
	 * main.c sentinel writes will advance 0xC5 → 0xA0 → 0xA1 → ... */
	li t0, 0xC5
	li t1, 0x20005800
	sb t0, 0(t1)

	mret
