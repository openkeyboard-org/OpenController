/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fault diagnosis handlers.
 *
 * The CH59x startup file declares NMI_Handler and HardFault_Handler as
 * weak symbols with a default `j 1b` infinite-loop body. These strong
 * definitions override them and capture fault state to fixed SRAM, then
 * either park or restart the module (see below).
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
 * What happens next is a build choice (KBD_FAULT_ACTION in the Makefile,
 * passed here as KBD_FAULT_RESET):
 *
 *   park (0, the default): stay in the handler for ever so a probe finds the
 *   chip as it faulted. Development only: the module is dead until it is
 *   reset or loses power.
 *
 *   reset (1): restart the module. The record above survives the restart
 *   (start-up never writes 0x20005804..0x20005813), so the UART diagnostics
 *   dump still reports where the fault was. For anything that ships.
 *
 * The park must survive the WWDG: main.c arms a reset-on-overflow
 * watchdog (~0.56 s @ 60 MHz) that keeps counting through a trap, which
 * would otherwise turn the park into a silent reboot loop. The restart
 * uses that same watchdog rather than RB_SOFTWARE_RESET, so start-up sees
 * RST_FLAG_WTR and keeps the pre-crash counters exactly as it does after
 * a hang (power_sleep.c); it arms the watchdog itself because a fault can
 * come before main.c has, or in a WATCHDOG_ENABLE=0 build.
 * In a reset build the start-up code also points mtvec at
 * fault_early_entry from its first instruction, because until it installs
 * the vector table a trap would otherwise land in the bootloader's own
 * trap loop and hang with nothing recorded.
 *
 * R8_RST_WDOG_CTRL is RWA ("SAM"): a store lands only inside the
 * safe-access window (0x57 then 0xA8 to R8_SAFE_ACCESS_SIG) - a bare
 * store is silently DISCARDED by hardware. QingKe V4 does not clear
 * mstatus.MIE on trap entry, but HardFault and NMI outrank every
 * interrupt, so nothing interleaves the unlock pair here (seen on the
 * bench: a parked module left an enabled, pending UART1 interrupt
 * unserved indefinitely). Neither path relies on that alone: the park
 * also feeds R8_WDOG_COUNT (plain RW) for ever, and the restart repeats
 * its whole sequence until the chip resets.
 * Clobbers t0-t2; the handlers never return.
 */

#ifndef KBD_FAULT_RESET
#define KBD_FAULT_RESET 0
#endif

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
	j fault_finish
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
	j fault_finish
	.size NMI_Handler, .-NMI_Handler

#if KBD_FAULT_RESET
	/* Trap entry for the start-up code, used from its first instruction
	 * until it installs the vector table (src/startup_CH592_phased.S).
	 * mtvec keeps its mode in the low two bits, so the entry is aligned
	 * to four bytes. Interrupts are not enabled yet, so whatever arrives
	 * here is a fault; mcause in the record says which. */
	.balign 4
	.global fault_early_entry
	.type fault_early_entry, @function
fault_early_entry:
	j HardFault_Handler
	.size fault_early_entry, .-fault_early_entry

	/* Shared restart. Inside the safe-access window (same idiom as the
	 * park below), enable the WWDG reset and disable its interrupt, whose
	 * weak handler would spin; then load the counter one step from
	 * overflow, so the reset comes 131072 system clocks later (about 2 ms
	 * at 60 MHz). Should a store ever be discarded, the loop comes round
	 * and does it all again: the wait is counted in instructions and is
	 * several times that prescaler period at any clock, so the retry can
	 * never keep reloading the counter ahead of its overflow. */
	.type fault_finish, @function
fault_finish:
	li t2, 0x40001040   /* R8_SAFE_ACCESS_SIG; +3 WDOG_COUNT, +6 RST_WDOG_CTRL */
1:	lbu t0, 6(t2)       /* R8_RST_WDOG_CTRL (reads are unrestricted) */
	andi t0, t0, -6     /* clear RB_WDOG_INT_EN; never write RB_SOFTWARE_RESET */
	ori t0, t0, 0x12    /* RB_WDOG_RST_EN | RW1C RB_WDOG_INT_FLAG */
	li t1, 0x57
	sb t1, 0(t2)        /* SAFE_ACCESS_SIG1 */
	nop
	nop
	li t1, 0xA8
	sb t1, 0(t2)        /* SAFE_ACCESS_SIG2: window open (~16 sysclks) */
	nop
	nop
	sb t0, 6(t2)        /* RWA store lands: WWDG overflow now resets */
	sb zero, 0(t2)      /* relock (SAFE_ACCESS_SIG0) */
	li t0, 0xFF
	sb t0, 3(t2)        /* R8_WDOG_COUNT: next count overflows */
	li t0, 0x100000     /* >= 2 M clocks: outlasts the 131072-clock count */
2:	addi t0, t0, -1
	bnez t0, 2b
	j 1b                /* still running: the store was lost, do it again */
	.size fault_finish, .-fault_finish
#else
	/* Shared diagnostic park. Disable the WWDG reset/interrupt inside the
	 * safe-access window (mirrors the SDK sys_safe_access_enable/disable
	 * idiom, incl. the SAFEOPERATE nop spacing), then park while feeding
	 * R8_WDOG_COUNT as insurance. All three registers sit at
	 * R8_SAFE_ACCESS_SIG (0x40001040) + {0, 3, 6}. */
	.type fault_finish, @function
fault_finish:
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
	.size fault_finish, .-fault_finish
#endif
