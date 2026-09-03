/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * App-owned deep-sleep module (power ladder MR5), replacing the SDK's
 * HAL/SLEEP.c under KBD_DEEP_SLEEP=1. Same two exported symbols the SDK
 * expects: HAL_SleepInit() (called from HAL_Init) and CH59x_LowPower()
 * (wired as the TMOS idle callback by MCU.c under HAL_SLEEP=TRUE).
 *
 * Why app-owned: the stock CH59x_LowPower would deep-sleep whenever TMOS is
 * idle -- INCLUDING while CONNECTED, because the connected-mode hop scheduler
 * is app-polled off raw RTC reads and invisible to TMOS. Every sleep here is
 * hard-gated (sleep => the RF link was already torn down; never sleep a live
 * link), and the whole module is RUNTIME-INERT until the UART sleep-protocol
 * rung sets pwr_autosleep (nothing in this rung sets it; the bench hook uses
 * its own explicit path).
 *
 * Wake sources: RTC trigger (armed per sleep with the caller's deadline;
 * RB_RTC_TRIG_EN is set only while a sleep is armed) and a GPIO falling edge
 * on the UART1 RX pin -- the CH592 has NO UART wake source (R8_SLP_WAKE_CTRL:
 * USB/RTC/GPIO/BAT only), so the host must lead with a discardable NULL
 * preamble; the waking byte is lost by design, and the sleep-protocol rung
 * must specify a post-NULL gap (the 8-byte UART FIFO holds ~700 us of
 * 115200 traffic against the hardware wake delay -- review finding). The
 * GPIO wake is armed ONLY across the sleep itself, never at init.
 *
 * Sleep executes with global IRQs masked end to end -- but NOT through the
 * SDK's LowPower_Sleep: its __WFI() clears WFITOWFE, and a QingKe WFI only
 * completes on an interrupt the controller can ACCEPT, so a masked WFI
 * sleeps forever (bench-proven on the shallow path; RB_WAKE_EV_MODE only
 * retains short wake pulses in the PMU, it does not change the CPU wait
 * semantics -- review finding). pwr_deep_commit() below forks the vendor
 * register sequence verbatim and waits via SEVONPEND WFE instead, the exact
 * mechanism bench-proven for the shallow idle: a wake source PENDING at the
 * PFIC ends the wait even while global delivery stays masked; handlers run
 * at the unmask. MR2's ordering discipline applies: the stale-event drain
 * runs BEFORE the final wake-source checks, never after.
 */
#include "CONFIG.h"
#include "HAL.h"
#include "keyboard_uart.h"
#include "rf_task.h"
#include "power_sleep.h"

#ifndef KBD_UART1_REMAP
#define KBD_UART1_REMAP 1
#endif
#ifndef KBD_SLEEP_BENCH_HOOK
#define KBD_SLEEP_BENCH_HOOK 0
#endif
#ifndef RF_DIAG_COUNTERS
#define RF_DIAG_COUNTERS 1
#endif
#ifndef WATCHDOG_ENABLE
#define WATCHDOG_ENABLE 1
#endif

/* The UART1 RX pin doubles as the GPIO wake source. */
#if KBD_UART1_REMAP
#define PWR_WAKE_PIN_IS_PB  1
#define PWR_WAKE_PIN        bRXD1_          /* PB12 */
#else
#define PWR_WAKE_PIN_IS_PB  0
#define PWR_WAKE_PIN        bRXD1           /* PA8 */
#endif

#if RF_DIAG_COUNTERS
/* .diag_safe.power: collected after every legacy counter (see ch592f.ld).
 * NOLOAD; the counters are zeroed in HAL_SleepInit, the bench phase byte
 * deliberately is NOT (it must survive a WWDG reset to report the verdict).
 * NOTE: the diag window is at capacity -- see the budget note in ch592f.ld
 * before adding anything here (review finding). */
volatile uint16_t pwr_sleep_attempt   __attribute__((section(".diag_safe.power")));
volatile uint16_t pwr_sleep_entered   __attribute__((section(".diag_safe.power")));
volatile uint16_t pwr_sleep_aborted   __attribute__((section(".diag_safe.power")));
volatile uint16_t pwr_wake_gpio       __attribute__((section(".diag_safe.power")));
volatile uint16_t pwr_wake_rtc        __attribute__((section(".diag_safe.power")));
volatile uint8_t  pwr_last_abort_reason __attribute__((section(".diag_safe.power")));
#define PWR_DIAG_INC16(x)   do { (x)++; } while (0)
#define PWR_ABORT(r)        do { pwr_last_abort_reason = (r); \
                                 pwr_sleep_aborted++; } while (0)
#else
#define PWR_DIAG_INC16(x)   do { } while (0)
#define PWR_ABORT(r)        do { } while (0)
#endif

/* Abort reasons (pwr_last_abort_reason). */
#define PWR_ABORT_RF        1u   /* radio not quiescent for deep sleep */
#define PWR_ABORT_TX        2u   /* UART TX not drained */
#define PWR_ABORT_RX        3u   /* UART RX bytes queued / parser mid-frame */
#define PWR_ABORT_OPENBOOT  4u   /* A6 81 bootloader entry pending */
#define PWR_ABORT_WINDOW    5u   /* requested window under min / over max */
#define PWR_ABORT_LATE      6u   /* RTC trigger already fired (stock rc 3) */
#define PWR_ABORT_RXLINE    7u   /* RX line low / byte in flight at boundary */

#if KBD_SLEEP_BENCH_HOOK
/* Single SWD-driven phase byte, NEVER cleared by firmware init so a WWDG
 * reset mid-sleep leaves an unambiguous verdict (review finding):
 *   0x01 written over SWD = run one bench sleep
 *   0x10 quiesced   0x20 entered (about to commit)   0x30 returned OK
 *   0x42 window rejected   0x43 boundary veto/late   0x44 TX-drain timeout
 * Phase 0x20 + ll_boot_count bump = the WWDG runs through deep sleep.
 * Phase 0x20 + no bump + no return = wake failed (external reset needed). */
volatile uint8_t pwr_bench_phase __attribute__((section(".diag_safe.power")));
#endif

/* Runtime enable. NOTHING in this rung sets it: the UART sleep-protocol rung
 * (A6 57 auto-sleep) is the only intended writer. */
static volatile uint8_t pwr_autosleep;

/* GPIO-wake activity latch (set on a consumed wake edge). */
static volatile uint8_t pwr_gpio_woke;

__attribute__((always_inline)) static inline uint32_t pwr_irq_save(void)
{
    uint32_t r;
    __asm volatile ("csrrc %0, 0x800, %1" : "=r"(r) : "r"(0x88) : "memory");
    return r & 0x88;
}
__attribute__((always_inline)) static inline void pwr_irq_restore(uint32_t s)
{
    __asm volatile ("csrrs zero, 0x800, %0" :: "r"(s) : "memory");
}

/* Modular RTC-tick helpers (RTC_MAX_COUNT modulus; review finding: a naive
 * base+ticks overflows the modulus in the last seconds before wrap). */
__HIGH_CODE
static uint32_t pwr_rtc_add(uint32_t base, uint32_t ticks)
{
    uint32_t t = base + ticks;
    if (t >= RTC_MAX_COUNT) {
        t -= RTC_MAX_COUNT;
    }
    return t;
}
__HIGH_CODE
static uint32_t pwr_rtc_sub(uint32_t base, uint32_t ticks)
{
    if (base >= ticks) {
        return base - ticks;
    }
    return base + (RTC_MAX_COUNT - ticks);
}
__HIGH_CODE
static uint32_t rtc_ticks_elapsed(uint32_t start)
{
    uint32_t now = RTC_GetCycle32k();
    if (now >= start) {
        return now - start;
    }
    return now + (RTC_MAX_COUNT - start);
}

/* Arm the RX-pin falling-edge wake. Boundary-only: called with IRQs masked
 * immediately before the sleep commit, undone immediately after wake. */
__HIGH_CODE
static void pwr_gpio_wake_arm(void)
{
#if PWR_WAKE_PIN_IS_PB
    R16_PB_INT_IF = (uint16_t)PWR_WAKE_PIN;          /* clear stale edge */
    GPIOB_ITModeCfg(PWR_WAKE_PIN, GPIO_ITMode_FallEdge);
    PFIC_ClearPendingIRQ(GPIO_B_IRQn);
    PFIC_EnableIRQ(GPIO_B_IRQn);
#else
    R16_PA_INT_IF = (uint16_t)PWR_WAKE_PIN;
    GPIOA_ITModeCfg(PWR_WAKE_PIN, GPIO_ITMode_FallEdge);
    PFIC_ClearPendingIRQ(GPIO_A_IRQn);
    PFIC_EnableIRQ(GPIO_A_IRQn);
#endif
    PWR_PeriphWakeUpCfg(ENABLE, RB_SLP_GPIO_WAKE, Long_Delay);
}

__HIGH_CODE
static void pwr_gpio_wake_disarm(void)
{
    PWR_PeriphWakeUpCfg(DISABLE, RB_SLP_GPIO_WAKE, Long_Delay);
#if PWR_WAKE_PIN_IS_PB
    PFIC_DisableIRQ(GPIO_B_IRQn);
    if (R16_PB_INT_IF & PWR_WAKE_PIN) {
        pwr_gpio_woke = 1;
        R16_PB_INT_IF = (uint16_t)PWR_WAKE_PIN;
    }
    R16_PB_INT_EN &= (uint16_t)~PWR_WAKE_PIN;
    PFIC_ClearPendingIRQ(GPIO_B_IRQn);
#else
    PFIC_DisableIRQ(GPIO_A_IRQn);
    if (R16_PA_INT_IF & PWR_WAKE_PIN) {
        pwr_gpio_woke = 1;
        R16_PA_INT_IF = (uint16_t)PWR_WAKE_PIN;
    }
    R16_PA_INT_EN &= (uint16_t)~PWR_WAKE_PIN;
    PFIC_ClearPendingIRQ(GPIO_A_IRQn);
#endif
}

/* Defensive handlers: the disarm path above clears pending state before the
 * unmask, so these should never run -- but the weak startup stubs spin
 * forever, which would be a WWDG reset. RAM-resident like every wake ISR. */
#if PWR_WAKE_PIN_IS_PB
__INTERRUPT
__HIGH_CODE
void GPIOB_IRQHandler(void)
{
    pwr_gpio_woke = 1;
    R16_PB_INT_IF = R16_PB_INT_IF;
}
#else
__INTERRUPT
__HIGH_CODE
void GPIOA_IRQHandler(void)
{
    pwr_gpio_woke = 1;
    R16_PA_INT_IF = R16_PA_INT_IF;
}
#endif

/* RTC trigger armed only while a sleep deadline is live (review finding: a
 * boot-armed trigger with the reset-value compare fires pointlessly once
 * per counter wrap). */
__HIGH_CODE
static void pwr_rtc_trig_arm(uint32_t time)
{
    RTC_SetTignTime(time);              /* also clears RTCTigFlag */
    sys_safe_access_enable();
    R8_RTC_MODE_CTRL |= RB_RTC_TRIG_EN;
    sys_safe_access_disable();
}
__HIGH_CODE
static void pwr_rtc_trig_disarm(void)
{
    sys_safe_access_enable();
    R8_RTC_MODE_CTRL &= ~RB_RTC_TRIG_EN;
    sys_safe_access_disable();
}

/* Deep-sleep commit: the vendor LowPower_Sleep register sequence transcribed
 * verbatim (DC-DC bits preserved, HSE bias raised, XT pre-start handling,
 * RB_RAM_RET_LV, (2<<11) plan field) EXCEPT the wait instruction: the
 * vendor __WFI() clears WFITOWFE, and a masked WFI never wakes on this
 * silicon, so the wait is a SEVONPEND WFE (SCTLR bits 4|3 armed by the
 * caller's drain step; SLEEPDEEP set here). Caller holds the global mask
 * and has already drained the stale event and re-checked every wake source.
 * RAM-resident; called only from pwr_sleep_until. */
__HIGH_CODE
__attribute__((noinline))
static void pwr_deep_commit(uint16_t rm)
{
    __attribute__((aligned(4))) uint8_t MacAddr[6] = {0};
    uint8_t x32Kpw, x32Mpw;
    uint16_t power_plan;

    GetMACAddress(MacAddr);             /* vendor flash-cmd quirk, kept */

    x32Kpw = R8_XT32K_TUNE;
    x32Mpw = R8_XT32M_TUNE;
    x32Mpw = (x32Mpw & 0xfc) | 0x03;    /* HSE bias 150% for wake */
    if (R16_RTC_CNT_32K > 0x3fff) {     /* past 500 ms: lower LSE bias */
        x32Kpw = (x32Kpw & 0xfc) | 0x01;
    }

    sys_safe_access_enable();
    R8_BAT_DET_CTRL = 0;
    sys_safe_access_disable();
    sys_safe_access_enable();
    R8_XT32K_TUNE = x32Kpw;
    R8_XT32M_TUNE = x32Mpw;
    sys_safe_access_disable();

    sys_safe_access_enable();
    R16_POWER_PLAN &= ~RB_XT_PRE_EN;
    sys_safe_access_disable();

    PFIC->SCTLR |= (1u << 2);           /* deep sleep; WFE mode already armed */

    power_plan = R16_POWER_PLAN & (RB_PWR_DCDC_EN | RB_PWR_DCDC_PRE);
    power_plan |= RB_PWR_PLAN_EN | RB_PWR_CORE | rm | (2u << 11);

    sys_safe_access_enable();
    R8_SLP_POWER_CTRL |= RB_RAM_RET_LV;
    R8_PLL_CONFIG |= (1u << 5);
    R16_POWER_PLAN = power_plan;
    sys_safe_access_disable();

    __asm__ volatile ("wfi");           /* executes as WFE; wakes on PENDING */
    __asm__ volatile ("nop");
    __asm__ volatile ("nop");

    PFIC->SCTLR &= ~((1u << 4) | (1u << 3) | (1u << 2));

    sys_safe_access_enable();
    R16_POWER_PLAN &= ~RB_XT_PRE_EN;
    sys_safe_access_disable();

    sys_safe_access_enable();
    R8_PLL_CONFIG &= ~(1u << 5);
    sys_safe_access_disable();
}

/* The actual sleep, shared by the idle callback and the bench hook. `time`
 * is the ABSOLUTE RTC tick to wake at. Stock return contract: 0 slept,
 * 2 window rejected, 3 did-not-sleep. Global mask held end to end. */
__HIGH_CODE
__attribute__((noinline))   /* never inline into a flash-resident caller */
static uint32_t pwr_sleep_until(uint32_t time)
{
    uint32_t time_sleep, time_curr;
    uint32_t irq_state;
    volatile uint32_t i;

    PWR_DIAG_INC16(pwr_sleep_attempt);

    /* Stock early-wake margin, modular (review finding). */
    time = pwr_rtc_sub(time, WAKE_UP_RTC_MAX_TIME);

    irq_state = pwr_irq_save();

    time_curr = RTC_GetCycle32k();
    if (time < time_curr) {
        time_sleep = time + (RTC_MAX_COUNT - time_curr);
    } else {
        time_sleep = time - time_curr;
    }
    if ((time_sleep < SLEEP_RTC_MIN_TIME) ||
        (time_sleep > SLEEP_RTC_MAX_TIME)) {
        pwr_irq_restore(irq_state);
        PWR_ABORT(PWR_ABORT_WINDOW);
        return 2;
    }

    pwr_rtc_trig_arm(time);
    pwr_gpio_woke = 0;
    pwr_gpio_wake_arm();

    /* MR2 ordering discipline: drain the stale WFE event BEFORE the final
     * wake-source checks (SEVONPEND only latches a NEW pending edge; a
     * source already pending when armed makes no event). Shallow here --
     * SLEEPDEEP is set only inside the commit. */
    PFIC->SCTLR &= ~(1u << 2);
    PFIC->SCTLR |= (1u << 4) | (1u << 3) | (1u << 5);   /* SEVONPEND|WFE|SEV */
    __asm__ volatile ("wfi");                           /* eats the self-SEV */
    PFIC->SCTLR |= (1u << 3);                           /* re-arm WFE mode */

    /* Boundary checks with SEVONPEND live: anything already pending is
     * caught here; anything later is a fresh edge the deep WFE catches.
     * RX line low = a byte is mid-flight whose edge already passed and
     * could NOT wake the sleep -- abort. */
    if (
#if PWR_WAKE_PIN_IS_PB
        (R32_PB_PIN & PWR_WAKE_PIN) == 0
#else
        (R32_PA_PIN & PWR_WAKE_PIN) == 0
#endif
            || R8_UART1_RFC != 0 || !KeyboardUart_RxQuiet() || pwr_gpio_woke) {
        PFIC->SCTLR &= ~((1u << 4) | (1u << 3));
        pwr_gpio_wake_disarm();
        pwr_rtc_trig_disarm();
        pwr_irq_restore(irq_state);
        PWR_ABORT(PWR_ABORT_RXLINE);
        return 3;
    }
    if (RTCTigFlag || (R8_RTC_FLAG_CTRL & RB_RTC_TRIG_FLAG)) {
        PFIC->SCTLR &= ~((1u << 4) | (1u << 3));
        pwr_gpio_wake_disarm();
        pwr_rtc_trig_disarm();
        pwr_irq_restore(irq_state);
        PWR_ABORT(PWR_ABORT_LATE);
        return 3;
    }

#if WATCHDOG_ENABLE
    WWDG_SetCounter(0);   /* full window from the instant sleep begins */
#endif
    PWR_DIAG_INC16(pwr_sleep_entered);
    pwr_deep_commit(RB_PWR_RAM2K | RB_PWR_RAM24K | RB_PWR_EXTEND | RB_XT_PRE_EN);

    /* Vendor-precedent wake path (the PM example calls HSECFG_Current right
     * after a GPIO wake at 60 MHz): the hardware Long_Delay wake latency
     * covers oscillator startup; no software settle theater on top of it
     * (review finding -- the previous settle loop called flash-resident SDK
     * code anyway, so it was never a flash-safety barrier). Stock one-tick
     * synchronization only. */
    HSECFG_Current(HSE_RCur_100);
    i = RTC_GetCycle32k();
    while (i == RTC_GetCycle32k()) { }

    if (R8_RTC_FLAG_CTRL & RB_RTC_TRIG_FLAG) {
        PWR_DIAG_INC16(pwr_wake_rtc);   /* hw flag: the ISR is still masked */
    } else {
        PWR_DIAG_INC16(pwr_wake_gpio);
    }

    pwr_gpio_wake_disarm();
    pwr_rtc_trig_disarm();
    pwr_irq_restore(irq_state);
    return 0;
}

/*********************************************************************
 * CH59x_LowPower -- the TMOS idle callback (MCU.c wires it under
 * HAL_SLEEP=TRUE). `time` is the next TMOS deadline as an absolute RTC
 * tick. Hard-gated and RUNTIME-INERT: pwr_autosleep is never set in this
 * rung. */
__HIGH_CODE
uint32_t CH59x_LowPower(uint32_t time)
{
    if (!pwr_autosleep) {
        return 3;   /* runtime-inert: MR5 never sleeps from the idleCB */
    }
    if (!RF_CanDeepSleep()) {
        PWR_ABORT(PWR_ABORT_RF);
        return 3;
    }
    if (OpenBoot_EntryPending()) {
        PWR_ABORT(PWR_ABORT_OPENBOOT);
        return 3;
    }
    if (!KeyboardUart_TxIdle()) {
        PWR_ABORT(PWR_ABORT_TX);
        return 3;
    }
    if (!KeyboardUart_RxQuiet()) {
        PWR_ABORT(PWR_ABORT_RX);
        return 3;
    }
    return pwr_sleep_until(time);
}

/*********************************************************************
 * HAL_SleepInit -- called once from HAL_Init. RTC wake source + IRQ only;
 * the trigger compare (RB_RTC_TRIG_EN) is armed per sleep, and the GPIO
 * wake is boundary-armed per sleep. */
void HAL_SleepInit(void)
{
    sys_safe_access_enable();
    R8_SLP_WAKE_CTRL |= RB_SLP_RTC_WAKE;
    sys_safe_access_disable();
    PFIC_EnableIRQ(RTC_IRQn);

#if RF_DIAG_COUNTERS
    pwr_sleep_attempt = 0;      /* .diag_safe.power is NOLOAD */
    pwr_sleep_entered = 0;
    pwr_sleep_aborted = 0;
    pwr_wake_gpio = 0;
    pwr_wake_rtc = 0;
    pwr_last_abort_reason = 0;
    /* pwr_bench_phase deliberately NOT cleared: it must survive a WWDG
     * reset to report the bench verdict (review finding). */
#endif
}

#if KBD_SLEEP_BENCH_HOOK
/*********************************************************************
 * Bench hook (EXTRA_CFLAGS-only build): one full-procedure 5 s deep sleep
 * on an SWD-poked request (write 0x01 to pwr_bench_phase), main-loop
 * context. Two-experiment protocol per the review: first prove masked WFE
 * wake with WATCHDOG_ENABLE=0, then answer WWDG-in-deep-sleep with the
 * watchdog on (phase 0x20 + boot-count bump = WWDG runs through sleep). */
__HIGH_CODE
void PowerSleep_BenchService(void)
{
    if (pwr_bench_phase != 0x01u) {
        return;
    }

    /* Quiesce per the OpenBoot_Service precedent: link down first (sleep =>
     * disconnect, the ladder's locked rule), then a bounded TX drain. */
    RF_FlushBondSave();
    RF_Disconnect();
    {
        uint32_t start = RTC_GetCycle32k();
        while (!KeyboardUart_TxIdle()) {
            if (rtc_ticks_elapsed(start) > 640u) {   /* ~20 ms at 32k */
                pwr_bench_phase = 0x44u;
                return;
            }
        }
    }
    pwr_bench_phase = 0x10u;   /* quiesced */

    {
        uint32_t deadline = pwr_rtc_add(RTC_GetCycle32k(), 5u * FREQ_RTC);
        pwr_bench_phase = 0x20u;   /* entering -- survives a WWDG reset */
        uint32_t rc = pwr_sleep_until(deadline);
        pwr_bench_phase = (rc == 0) ? 0x30u : (rc == 2) ? 0x42u : 0x43u;
    }
}
#endif
