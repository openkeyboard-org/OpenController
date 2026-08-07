/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 *
 * Fault diagnosis handlers.
 *
 * The CH59x startup file declares NMI_Handler and HardFault_Handler as
 * weak symbols with a default `j 1b` infinite-loop body. These strong
 * definitions override them and capture fault state to fixed SRAM before
 * disabling the WWDG reset path and halting, so the next SWD read sees
 * the fault PC.
 *
 * Diagnostic SRAM layout (chosen above main.c's boot_sentinel and below
 * BSS, so it survives soft-reset clearing of .diag_safe-style NOLOAD):
 *
 *   0x20005800  byte  boot_sentinel    — phase markers (0xC0..0xC5 → 0xA0+ → 0xB0+)
 *   0x20005801  byte  reset_status     — R8_RESET_STATUS captured in startup phase 0xC0
 *   0x20005802  byte  — padding
 *   0x20005803  byte  — padding
 *   0x20005804  byte  fault_marker     — 0xDE = HardFault, 0xDF = NMI
 *   0x20005808  word  fault_mepc       — fault PC (mepc CSR)
 *   0x2000580C  word  fault_mcause     — fault cause (mcause CSR)
 *   0x20005810  word  fault_mtval      — fault value (mtval CSR, addr if applicable)
 *
 * Placed in .text (flash) so the handlers are reachable even before the
 * .highcode flash→SRAM copy completes — a fault in early startup still
 * lands in valid code. The vector table entries (`.word HardFault_Handler`)
 * resolve at link time to flash addresses, so mtvec mode-3 works.
 *
 * The park must survive the WWDG: main.c arms a reset-on-overflow
 * watchdog (~0.56 s @ 60 MHz) that keeps counting through a trap, which
 * would otherwise turn the park into a silent reboot loop.
 * R8_RST_WDOG_CTRL is RWA ("SAM"): a store lands only inside the
 * safe-access window (0x57 then 0xA8 to R8_SAFE_ACCESS_SIG) — a bare
 * store is silently DISCARDED by hardware. Trap entry cleared
 * mstatus.MIE, so nothing can interleave the unlock pair here. Belt and
 * braces, the park loop also feeds R8_WDOG_COUNT (plain RW) forever, so
 * the park holds even if the RWA disable were ever ineffective.
 * Clobbers t0-t2; the handlers never return.
 */

	.section .text, "ax", @progbits
	.align 1

	.global HardFault_Handler
	.global NMI_Handler

	.type HardFault_Handler, @function
HardFault_Handler:
	li t0, 0xDE
	li t1, 0x20005804
	sb t0, 0(t1)
	csrr t0, mepc
	sw t0, 4(t1)        /* 0x20005808 = mepc */
	csrr t0, mcause
	sw t0, 8(t1)        /* 0x2000580C = mcause */
	csrr t0, mtval
	sw t0, 12(t1)       /* 0x20005810 = mtval */
	j fault_wdog_park
	.size HardFault_Handler, .-HardFault_Handler

	.type NMI_Handler, @function
NMI_Handler:
	li t0, 0xDF
	li t1, 0x20005804
	sb t0, 0(t1)
	csrr t0, mepc
	sw t0, 4(t1)
	csrr t0, mcause
	sw t0, 8(t1)
	csrr t0, mtval
	sw t0, 12(t1)
	j fault_wdog_park
	.size NMI_Handler, .-NMI_Handler

	/* Shared diagnostic park. Disable the WWDG reset/interrupt inside the
	 * safe-access window (mirrors the SDK sys_safe_access_enable/disable
	 * idiom, incl. the SAFEOPERATE nop spacing), then park while feeding
	 * R8_WDOG_COUNT as insurance. All three registers sit at
	 * R8_SAFE_ACCESS_SIG (0x40001040) + {0, 3, 6}. */
	.type fault_wdog_park, @function
fault_wdog_park:
	li t2, 0x40001040   /* R8_SAFE_ACCESS_SIG; +3 WDOG_COUNT, +6 RST_WDOG_CTRL */
	lbu t0, 6(t2)       /* R8_RST_WDOG_CTRL (reads are unrestricted) */
	andi t0, t0, -7     /* clear RB_WDOG_RST_EN | RB_WDOG_INT_EN */
	ori t0, t0, 0x10    /* RW1C RB_WDOG_INT_FLAG if already pending */
	li t1, 0x57
	sb t1, 0(t2)        /* SAFE_ACCESS_SIG1 */
	nop
	nop
	li t1, 0xA8
	sb t1, 0(t2)        /* SAFE_ACCESS_SIG2: window open (~16 sysclks) */
	nop
	nop
	sb t0, 6(t2)        /* RWA store lands: WWDG reset/interrupt disabled */
	sb zero, 0(t2)      /* relock (SAFE_ACCESS_SIG0) */
1:	sb zero, 3(t2)      /* perpetual R8_WDOG_COUNT feed (plain RW) */
	j 1b
	.size fault_wdog_park, .-fault_wdog_park
