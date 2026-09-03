/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * OpenController keyboard wireless-module firmware for the WCH CH592F.
 *
 * This is intentionally a keyboard-module application, not a USB dongle:
 * UART1 uses the selected board profile's pin mapping and speaks the
 * wireless-module binary command/status protocol used by the keyboard's
 * host MCU.
 */

#include "CONFIG.h"
#include "HAL.h"
#include "keyboard_uart.h"
#include "rf_task.h"
#include "openboot_app.h"

/* Run the core from the CH592's DC-DC converter instead of the LDO.
 * Bench-measured 2026-09-02 at 5.03 mA vs 7.17 mA active (-29.8%), see the
 * enabling commit. This is a board hardware claim owned by the board
 * profile (boards/*.mk, validated at parse time): the board must populate
 * the DC-DC inductor -- PWR_DCDCCfg only declines on silicon that cannot do
 * DC-DC at all (ROM_CFG_ADR_HW bit 13), never on a board that simply lacks
 * the part, and without the inductor the core supply collapses. The
 * fallback below covers non-Makefile builds only. */
#ifndef KBD_DCDC_ENABLE
#define KBD_DCDC_ENABLE 1
#endif

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if (defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0xbd, 0xc3, 0xab, 0x10, 0x53, 0x5c};
#endif

static uint8_t transport_is_2g4;

/* Latched by the A6 81 UART command, acted on by OpenBoot_Service(). */
static volatile uint8_t openboot_entry_pending;

#if KBD_DEEP_SLEEP
#include "power_sleep.h"
/* Exported for the deep-sleep gate: a pending bootloader entry vetoes
 * sleep (the update path expects a running main loop). */
uint8_t OpenBoot_EntryPending(void)
{
    return openboot_entry_pending;
}
#endif

/* Independent watchdog (WWDG): last-resort recovery for an unknown hang in
 * Main_Circulation. Reset-only -- the WWDG interrupt is left disabled because the
 * weak WDOG_BAT_IRQHandler spins forever. Fed every loop iteration; at Fsys/131072
 * (60 MHz) the max window is ~0.56 s, far above any legitimate loop iteration.
 * Gated by WATCHDOG_ENABLE so a build can opt out. */
#ifndef WATCHDOG_ENABLE
#define WATCHDOG_ENABLE 1
#endif
#if WATCHDOG_ENABLE
#define WWDG_FEED_VALUE 0u
static void watchdog_init(void)
{
    WWDG_ITCfg(DISABLE);
    WWDG_ClearFlag();
    WWDG_SetCounter(WWDG_FEED_VALUE);
    WWDG_ResetCfg(ENABLE);
    WWDG_SetCounter(WWDG_FEED_VALUE);
}
#define WATCHDOG_FEED()  WWDG_SetCounter(WWDG_FEED_VALUE)
#else
static void watchdog_init(void) { }
#define WATCHDOG_FEED()  ((void)0)
#endif

static void handle_uart_frame(uint8_t cmd, uint8_t sub,
                              const uint8_t *payload, uint8_t len)
{
    (void)len;

    if (cmd == 0xA1) {
        RF_QueueHIDReport(payload);
        return;
    }

    if (cmd == 0xA9) {
        /* BLE device-name frame. Accepted for protocol compatibility;
         * the BLE HID advertising path is not implemented yet. */
        return;
    }

    if (cmd != 0xA6) {
        return;
    }

    switch (sub) {
    case 0x11: /* select USB */
        transport_is_2g4 = 0;
        RF_Disconnect();
        KeyboardUart_SendStatus(0x34);
        KeyboardUart_SendStatus(0x36);
        break;

    case 0x30: { /* select 2.4G */
        uint8_t has_bond;
        transport_is_2g4 = 1;
        has_bond = RF_Select2G4();
        KeyboardUart_SendStatus(0x34);
        KeyboardUart_SendStatus(has_bond ? 0x35 : 0x36);
        break;
    }

    case 0x31: /* select BT1 */
    case 0x32: /* select BT2 */
    case 0x33: /* select BT3 */
        transport_is_2g4 = 0;
        RF_Disconnect();
        KeyboardUart_SendStatus(0x34);
        KeyboardUart_SendStatus(0x36);
        KeyboardUart_SendStatus(0x23);
        break;

    case 0x51: /* pair current transport */
        if (transport_is_2g4) {
            if (!RF_IdentityValid()) {
                KeyboardUart_SendStatus(0x36);
                break;
            }
            RF_EnterPairing();
            KeyboardUart_SendStatus(0x31);
            KeyboardUart_SendStatus(0x23);
        }
        break;

    case 0x52: /* unpair */
        RF_Disconnect();
        RF_ClearBond();
        KeyboardUart_SendStatus(0x33);
        break;

    case 0x53: /* battery */
        KeyboardUart_SendBattery(100);
        break;

    case 0x54: /* sleep */
    case 0x55: /* sleep-bt-en */
    case 0x57: /* sleep-2g4-en */
    case 0x62: /* factory BT pair */
        break;

    case 0x63: /* factory 2.4G pair */
        transport_is_2g4 = 1;
        if (!RF_IdentityValid()) {
            KeyboardUart_SendStatus(0x36);
            break;
        }
        RF_Disconnect();
        RF_ClearBond();
        RF_EnterPairing();
        KeyboardUart_SendStatus(0x31);
        KeyboardUart_SendStatus(0x23);
        break;

    case 0x70: /* version: stock firmwareB often ACKs only */
        break;

    case 0x81: /* OTA mode -> enter the OpenBoot bootloader.
                * Latch only; OpenBoot_Service() acts from the main loop so
                * the frame's ACK is queued first and RF teardown never nests
                * inside frame dispatch. Single-shot is fail-safe here: with a
                * blessed image, a spurious trigger costs ~10 s off-air and
                * the bootloader's idle timeout boots the app back. */
        openboot_entry_pending = 1;
        break;

    default:
        break;
    }
}

/* Enter-bootloader service, one step per Main_Circulation pass (mirrors the
 * OpenDongle IAP_Service split: the UART dispatch only LATCHES, this acts).
 *
 *   QUIESCE — RF_FlushBondSave() writes a pending deferred bond save
 *             synchronously (an A6 81 right after a fresh pair must not
 *             lose the bond; RF_Disconnect() clears the pending flag), then
 *             RF_Disconnect(): TMR0 stopped, RF tasks stopped, RF_Shut;
 *   DRAIN   — wait for the frame's 61 0D 0A ACK to physically leave the
 *             UART (bounded ~20 ms wall clock: a host not draining must
 *             not block the update);
 * then mask global IRQs (CSR 0x800, same idiom as rf_task's critical
 * sections — nothing may re-arm the radio past this point) and enter the
 * bootloader via openboot_request_update() (writes OB_BOOTREQ_MAGIC to the
 * reserved top-of-RAM word and software-resets; noreturn). */
static void OpenBoot_Service(void)
{
    static uint8_t  svc_state;
    static uint32_t svc_start;

    switch (svc_state) {
    case 0:
        if (!openboot_entry_pending) {
            return;
        }
        RF_FlushBondSave();
        RF_Disconnect();
        svc_start = SYS_GetSysTickCnt();
        svc_state = 1;
        return;
    default:
        if (!KeyboardUart_TxIdle()
                && (uint32_t)(SYS_GetSysTickCnt() - svc_start)
                       < (GetSysClock() / 50u)) {   /* ~20 ms */
            return;
        }
        __asm volatile ("csrrc zero, 0x800, %0" :: "r"(0x88) : "memory");
        openboot_request_update();  /* noreturn */
    }
}

#if KBD_IDLE_WFI
/* Idle-state WFI (power ladder MR2). Heartbeat: WWDG counts whenever Fsys
 * runs -- idle mode included -- so an idle keyboard with no UART traffic
 * and no other IRQ must still wake inside the ~559 ms watchdog window to
 * feed it. 200 ms leaves >2x margin. TMR3 is raw-register one-purpose use,
 * mirroring rf_task's TMR0 idiom; the later clock-gating rung must never
 * gate TMR3 for this reason. The handler only clears the flag: waking IS
 * the point. */
static void heartbeat_init(void)
{
    R8_TMR3_CTRL_MOD = RB_TMR_ALL_CLEAR;
    R32_TMR3_CNT_END = GetSysClock() / 5u;   /* 200 ms */
    R8_TMR3_INT_FLAG = RB_TMR_IF_CYC_END;
    R8_TMR3_INTER_EN = RB_TMR_IE_CYC_END;
    R8_TMR3_CTRL_MOD = RB_TMR_COUNT_EN;
    PFIC_EnableIRQ(TMR3_IRQn);
}

__INTERRUPT
__HIGH_CODE
void TMR3_IRQHandler(void)
{
    R8_TMR3_INT_FLAG = RB_TMR_IF_CYC_END;
}

#ifndef KBD_SLEEP_BENCH_HOOK
#define KBD_SLEEP_BENCH_HOOK 0
#endif
#ifndef RF_DIAG_COUNTERS
#define RF_DIAG_COUNTERS 1
#endif
#if RF_DIAG_COUNTERS
/* .diag_safe.power: the linker collects subsections AFTER every exact-name
 * .diag_safe section, so power counters never shift the legacy rf_task/
 * keyboard_uart counter addresses bench scripts read by absolute address
 * (guarded by an ASSERT in ch592f.ld). NOLOAD: zeroed in main(). */
volatile uint32_t pwr_wfi_count
    __attribute__((section(".diag_safe.power")));
#define PWR_DIAG_INC(x) do { (x)++; } while (0)
#else
#define PWR_DIAG_INC(x) do { } while (0)
#endif
#endif /* KBD_IDLE_WFI */

#if KBD_IDLE_WFI
/* Idle via WFE (WFI reinterpreted as wait-for-event, SCTLR WFITOWFE bit 3)
 * with SEVONPEND (bit 4), NOT a masked WFI: a WFI entered with the global-IRQ
 * CSR masked never wakes on a pending source on this silicon -- the masked
 * WFI slept to the WWDG reset and reboot-looped (~1 boot/2.7 s; this also
 * invalidated the first "1.75 mA idle" figure, which was that loop's
 * average). Under SEVONPEND an interrupt going pending is a wake event even
 * while global delivery stays masked.
 *
 * The split into arm-drain / sleep is load-bearing, not stylistic. SEVONPEND
 * on this core only latches a NEW pending edge; a source ALREADY pending when
 * SEVONPEND is enabled generates no event. So the stale-event drain (the
 * self-SEV + WFE that the SDK __WFE folds together) must run BEFORE the final
 * wake-source check, not after it: otherwise a UART byte or TMR3 heartbeat
 * that pends in the window between the check and the drain is neither seen by
 * the check nor edge-detected by SEVONPEND, and the real WFE can sleep past
 * it to the WWDG reset (a TMR3 cycle-end is the dangerous case -- its RW1
 * flag stays set with the ISR masked, so it produces no later edge). This
 * ordering was the adversarial-review blocker on the single-function form.
 *
 * Sequence (all under the caller's CSR-0x800 mask):
 *   idle_arm_and_drain(): arm WFE+SEVONPEND, self-SEV, one WFE -> latch clean
 *   caller re-checks every wake source, INCLUDING the TMR3 flag, with
 *     SEVONPEND now live: anything already pending is caught here; anything
 *     that pends later is a fresh edge the real WFE below catches.
 *   idle_sleep_once(): flash off (until next fetch), one real WFE.
 *   idle_disarm(): clear WFE+SEVONPEND. */
__HIGH_CODE
static void idle_arm_and_drain(void)
{
    PFIC->SCTLR &= ~(1u << 2);                        /* sleep, not deep */
    PFIC->SCTLR |= (1u << 4) | (1u << 3) | (1u << 5); /* SEVONPEND|WFE|SEV */
    __asm__ volatile ("wfi");                         /* self-SEV -> drains */
    PFIC->SCTLR |= (1u << 3);                         /* re-arm WFE mode */
}

__HIGH_CODE
static void idle_sleep_once(void)
{
    FLASH_ROM_SW_RESET();
    R8_FLASH_CTRL = 0x04;                             /* flash off til fetch */
    __asm__ volatile ("wfi");                         /* real wait */
}

__HIGH_CODE
static void idle_disarm(void)
{
    PFIC->SCTLR &= ~((1u << 4) | (1u << 3));
}
#endif

__HIGH_CODE
__attribute__((noinline))
void Main_Circulation(void)
{
    while (1) {
        TMOS_SystemProcess();
        RF_ConnectedTick();
        KeyboardUart_Poll();
        OpenBoot_Service();
#if KBD_DEEP_SLEEP && KBD_SLEEP_BENCH_HOOK
        PowerSleep_BenchService();
#endif
        WATCHDOG_FEED();
#if KBD_IDLE_WFI
        /* Idle the core only in RF_STATE_IDLE: TMOS scheduling and the hop
         * servo are POLL-driven, so PAIRING (20 ms beacon cadence) and
         * CONNECTED (~875 us hop grid) must keep spinning. rf_state only
         * changes in main-loop context (TMOS handlers), so it needs no
         * re-check below. */
        if (RF_GetState() == RF_STATE_IDLE && !openboot_entry_pending
                && KeyboardUart_RxQuiet()) {
            uint32_t irq_state;
            /* Global-mask critical section (CSR 0x800 MPIE|MIE, rf_task.c
             * idiom). Under the mask: drain any stale event FIRST, then
             * re-check every wake source with SEVONPEND live, then sleep.
             * The re-check covers the ring/FIFO (a byte that pended before
             * the mask), OpenBoot, and the TMR3 heartbeat flag -- its ISR
             * cannot run while masked, so a heartbeat that already pended
             * would be an edge-less source the WFE could sleep past to the
             * WWDG reset. A source that pends AFTER this check is a fresh
             * edge the WFE catches. Woken handlers run at the csrrs. */
            __asm volatile ("csrrc %0, 0x800, %1"
                            : "=r"(irq_state) : "r"(0x88) : "memory");
            idle_arm_and_drain();
            if (KeyboardUart_RxQuiet() && R8_UART1_RFC == 0
                    && !openboot_entry_pending
                    && (R8_TMR3_INT_FLAG & RB_TMR_IF_CYC_END) == 0) {
                WATCHDOG_FEED();
                PWR_DIAG_INC(pwr_wfi_count);
                idle_sleep_once();
            }
            idle_disarm();
            __asm volatile ("csrrs zero, 0x800, %0"
                            :: "r"(irq_state & 0x88) : "memory");
        }
#endif
    }
}

/* Boot-phase sentinel at 0x20005800, continuing the startup's 0xC0..0xC5
 * series (see startup_CH592_phased.S): 0xA0.. marks main()'s init phases so
 * a wedge is SWD-attributable without a debugger. */
#define BOOT_PHASE(x)  (*(volatile uint8_t *)0x20005800 = (uint8_t)(x))

int main(void)
{
    BOOT_PHASE(0xA0);
#if KBD_DCDC_ENABLE
    /* Ahead of SetSysClock, matching WCH's own BLE examples: the switch is a
     * supply transient, and taking it while the core still runs from the
     * reset-default clock keeps it away from the PLL. Requires the board to
     * populate the DC-DC inductor (board profile knob) -- PWR_DCDCCfg only
     * declines on silicon that cannot do DC-DC at all (ROM_CFG_ADR_HW bit
     * 13), never on a board that simply lacks the part. */
    PWR_DCDCCfg(ENABLE);
#endif
    SetSysClock(CLK_SOURCE_PLL_60MHz);
    BOOT_PHASE(0xA1);

    /* Park every pin as a pulled-up input before any peripheral claims its
     * own (WCH's own examples do the same ahead of low-power use): a
     * floating CMOS input can sit mid-rail and burn crossbar current
     * continuously. KeyboardUart_Init re-claims the UART pins immediately
     * below. PB13 is excluded from the park: on the MK65MX profile it is
     * CHWAKE, driven push-pull by the keyboard host, and must never be
     * biased even transiently (review finding); on the remap profile it is
     * this firmware's own TX pin and is driven high a few lines down. */
    GPIOA_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_All & ~bTXD1_, GPIO_ModeIN_PU);

    KeyboardUart_Init();
    KeyboardUart_SetFrameCallback(handle_uart_frame);
    BOOT_PHASE(0xA2);

    /* Clear OpenBoot-inherited SysTick PENDING state. The bootloader uses
     * SysTick for its idle timeout and stops the counter before jumping
     * here (ob_jump_app), but stopping does not clear an already-latched
     * count-flag or the PFIC pending bit. CH59x_BLEInit's SysTick_Config
     * enables the SysTick IRQ one line before PFIC disables it, and the
     * inherited pending state fires in that window, vectoring into the
     * startup's weak infinite-loop handler (bench-diagnosed: boot phase
     * parked at the pre-BLEInit marker; this clear cures it). */
    SysTick->CTLR = 0;
    SysTick->SR = 0;
    PFIC_ClearPendingIRQ(SysTick_IRQn);

    CH59x_BLEInit();
    BOOT_PHASE(0xA3);
    HAL_Init();
    BOOT_PHASE(0xA4);

    /* Power ladder MR3: mark every unused peripheral's clock for gating in
     * R32_SLEEP_CONTROL. Semantics note (inverted API): DISABLE *sets* the
     * SLP_CLK bits = clock off while the core sleeps -- and per CH592
     * datasheet section 5.3 that includes Idle (SLEEPDEEP=0, WFI/WFE), so
     * with the WFE idle from the previous rung this should show up as an
     * S2 idle delta; awake current is unchanged. Never gate: TMR0
     * (connected-mode turnaround one-shot), TMR3 (WFE watchdog heartbeat),
     * UART1 (host link + wake source), BLE (radio). I2C and LCD have no
     * BIT_SLP_CLK_* convenience macros but their SFR-level byte-1 bits
     * exist and both peripherals are unused (review finding) -- gated via
     * the shifted RB_ forms. SPI1's documented bit position (0x0200) is
     * deliberately NOT set: the CH592 headers comment the SPI1 peripheral
     * out (not present on this part), and poking a bit the headers refuse
     * to name buys nothing measurable. */
#if (RB_SLP_CLK_I2C | RB_SLP_CLK_LCD) > 0xFF
#error "RB_SLP_CLK_I2C/LCD must be byte-local masks for the <<8 shift below"
#endif
    PWR_PeriphClkCfg(DISABLE,
                     BIT_SLP_CLK_TMR1 | BIT_SLP_CLK_TMR2
                     | BIT_SLP_CLK_UART0 | BIT_SLP_CLK_UART2
                     | BIT_SLP_CLK_UART3 | BIT_SLP_CLK_SPI0
                     | BIT_SLP_CLK_PWMX | BIT_SLP_CLK_USB
                     | (uint16_t)((uint16_t)(RB_SLP_CLK_I2C
                                             | RB_SLP_CLK_LCD) << 8));

    RF_RoleInit();
    BOOT_PHASE(0xA5);

    /* No manual PFIC_EnableIRQ(BLEB_IRQn)/PFIC_EnableIRQ(BLEL_IRQn) here: the BLE
     * library's BLE_IPCoreInit already writes the IRQ 20/21 enables, so the
     * app-side calls were redundant. Removing them was re-validated on the v1.4.2
     * lib under OpenOCD -- connects, 100% HID delivery, 348k clean connected hops,
     * 0 HardFaults. (The earlier "required -- removing them faults TMOS time
     * processing" belief was a minichlink SDI-reset measurement artifact: the
     * reset itself induces the fault at boot, not the missing enables.) No app
     * BB_IRQHandler is needed either: the library fast-vectors BLEB to
     * BB_IRQLibFunction in hardware (see the Makefile SCHED_SRC note). */

    RF_TaskInit();
    BOOT_PHASE(0xA6);
    if (RF_HasBond() && RF_IdentityValid()) {
        transport_is_2g4 = 1;
        KeyboardUart_SendStatus(0x34);
        KeyboardUart_SendStatus(0x35);
    }
    watchdog_init();
#if KBD_IDLE_WFI
#if RF_DIAG_COUNTERS
    pwr_wfi_count = 0;   /* .diag_safe.power is NOLOAD; startup never clears it */
#endif
    heartbeat_init();
#endif
    BOOT_PHASE(0xA7);
    Main_Circulation();
}
