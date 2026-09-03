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
 * link). Entry paths: PowerSleep_ExplicitOnce() (MR6, host commands it with
 * A6 54); and, once the host arms auto-sleep with A6 57 (MR7), two
 * autonomous sites partitioned by RF state -- PowerSleep_IdleAutosleep()
 * from the main loop in RF IDLE (GPIO-only, indefinite "sleep until the
 * host talks"), and the TMOS idle callback CH59x_LowPower in the radio-off
 * slices of a bonded reconnect search (RTC-bounded by the 20 ms beacon
 * deadline TMOS hands it). The split means IDLE sleep never depends on what
 * deadline TMOS passes when it has nothing scheduled (undocumented).
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
 * Entry runs masked (window math, wake arming, boundary checks), but the
 * deep sleep itself is the STOCK SDK LowPower_Sleep (plain __WFI) entered
 * UNMASKED. Two bench experiments killed the first design (a fork of
 * LowPower_Sleep with a SEVONPEND-WFE wait): it wedged the part whether the
 * wait ran masked or unmasked, and only a power cycle recovered it. Root
 * cause (Codex root-cause pass, confirmed by the vendor's own working
 * path): WFE-mode (WFITOWFE) is incompatible with SLEEPDEEP=1 on this core.
 * The MR2 shallow-idle lesson -- masked WFI hangs, masked WFE wakes -- does
 * NOT transfer to deep sleep. So the deep wait is a plain WFI, which wakes
 * only on an interrupt the controller can ACCEPT, hence unmasked; the
 * vendor routine is left byte-for-byte (it also owns undocumented PLL /
 * power-plan bits we must not transcribe). RTC_IRQn and the RX-pin GPIO IRQ
 * are the accepted wakes.
 *
 * The check-to-WFI race for ASYNC UART/GPIO wake is not closed at this
 * layer -- a single NULL edge can be consumed by its ISR inside the sleep
 * prologue. Formal closure is a HELD BREAK or a repeated preamble, owned by
 * the UART sleep-protocol rung. The RTC deadline is race-safe by
 * SLEEP_RTC_MIN_TIME, and this rung's runtime path is inert with an
 * RTC-driven bench, so the async race is out of scope here.
 */
#include "CONFIG.h"
#include "HAL.h"
#include "keyboard_uart.h"
#include "rf_task.h"
#include "power_sleep.h"
#include "sleep_protocol.h"

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
 * NOTE: the diag window is the fixed 0x200-byte region asserted in
 * ch592f.ld (_diag_safe_end <= _diag_scratch_start + 0x200). The hook build
 * ends it at 0x200059fc -- ~4 bytes of headroom -- so adding retained state
 * here risks tripping that ASSERT; check the map before doing so. */
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
/* Bench phase byte, NEVER cleared by firmware init so a WWDG reset
 * mid-sleep leaves a record that survives a warm reset (review finding).
 * Driven over UART: A6 54 -> PowerSleep_BenchRequest sets 0x01. SWD reads
 * cannot observe a sleeping core and NOLOAD RAM is indeterminate after the
 * power-cycle recovery, so the bench is UART-triggered and the meter +
 * UART liveness are the witnesses; this byte is a post-mortem aid only.
 *   0x01 requested   0x10 quiesced   0x20 entered   0x30 returned OK
 *   0x42 window rejected   0x43 boundary veto/late   0x44 TX-drain timeout
 * Phase 0x20 + ll_boot_count bump = the WWDG runs through deep sleep.
 * Phase 0x20 + no bump + no return = wake failed (external reset needed). */
volatile uint8_t pwr_bench_phase __attribute__((section(".diag_safe.power")));
#endif

/* Auto-sleep enable, mirrored from the sleep-protocol reducer (A6 57 arms,
 * A6 56/51/52/63 and boot clear). */
static volatile uint8_t pwr_autosleep;

/* GPIO-wake activity latch (set on a consumed wake edge). */
static volatile uint8_t pwr_gpio_woke;

/* Post-activity holdoff (review #12): after a GPIO wake OR any accepted UART
 * frame, autonomous sleep is refused for KBD_AUTOSLEEP_HOLDOFF_MS so the
 * host's post-NULL frame -- and any burst of frames after it -- lands on an
 * awake module. Only the FIRST frame after silence needs the preamble. */
#ifndef KBD_AUTOSLEEP_HOLDOFF_MS
#define KBD_AUTOSLEEP_HOLDOFF_MS 100u
#endif
static uint32_t pwr_holdoff_start;
static volatile uint8_t pwr_holdoff_active;

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

/* Vendor LowPower_Sleep + the HSE-bias restore it omits. The primitive
 * raises R8_XT32M_TUNE to 150% for a reliable wake and never lowers it; the
 * SDK's own CH59x_LowPower wrapper calls HSECFG_Current(HSE_RCur_100) right
 * after return. Omitting it leaves ACTIVE current elevated after the first
 * wake -- this was the 1.60 mA post-wake seen on the MR5 bench, not
 * parasitics (review finding). Both sleep paths go through here. */
__HIGH_CODE
static void pwr_low_power_sleep(uint16_t rm)
{
    LowPower_Sleep(rm);
    HSECFG_Current(HSE_RCur_100);
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
/* Same 1024-tick backstep allowance as rf_task.c's RTC32K_BACKSTEP_TOLERANCE:
 * a small backward RTC sample must read as zero elapsed, not as a ~24 h wrap
 * that would expire a fresh holdoff instantly (review finding). */
#define PWR_RTC32K_BACKSTEP_TOLERANCE 1024u
static uint32_t rtc_ticks_elapsed(uint32_t start)
{
    return SleepProtocol_ElapsedTicks(RTC_GetCycle32k(), start,
                                      RTC_MAX_COUNT, PWR_RTC32K_BACKSTEP_TOLERANCE);
}

static void pwr_note_activity(void)
{
    pwr_holdoff_start = RTC_GetCycle32k();
    pwr_holdoff_active = 1;
}

/* 1 when autonomous sleep may proceed w.r.t. recent host activity. */
static uint8_t pwr_holdoff_expired(void)
{
    /* Any raw RX byte since the last check -- a discarded NULL preamble, a
     * host ACK, noise -- is host activity the frame callback never saw. */
    if (KeyboardUart_TakeRxActivity()) {
        pwr_note_activity();
        return 0;
    }
    if (!pwr_holdoff_active) {
        return 1;
    }
    if (rtc_ticks_elapsed(pwr_holdoff_start) >= MS_TO_RTC(KBD_AUTOSLEEP_HOLDOFF_MS)) {
        pwr_holdoff_active = 0;
        return 1;
    }
    return 0;
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
    /* Clear the trigger flag and PFIC pending so the unmask cannot re-enter
     * an RTC IRQ. Defensive: the SDK's HAL/RTC.c RTC_IRQHandler is linked
     * (RAM-resident, clears the flag itself), so the "weak stub loops
     * forever" premise does not hold here -- but on a GPIO wake the RTC
     * trigger may fire in the wake-to-disarm window, and clearing it here
     * keeps correctness independent of which handler is linked (review). */
    R8_RTC_FLAG_CTRL = RB_RTC_TRIG_CLR;
    PFIC_ClearPendingIRQ(RTC_IRQn);
}

/* The actual sleep, shared by the idle callback and the bench hook. `time`
 * is the ABSOLUTE RTC tick to wake at. Return contract: 0 slept, 2 window
 * rejected, 3 did-not-sleep.
 *
 * Deep-sleep entry is the STOCK SDK LowPower_Sleep (plain __WFI), entered
 * with global interrupts UNMASKED. Bench experiments proved the forked
 * SEVONPEND-WFE deep path wedges the part regardless of masking: WFE-mode
 * (WFITOWFE) is incompatible with SLEEPDEEP=1 on this core -- neither RTC
 * nor GPIO could wake it, only a power cycle recovered it. The shallow-idle
 * lesson (masked WFE wakes) does NOT transfer to deep sleep. So the wait is
 * a plain WFI, which wakes on any interrupt the controller can ACCEPT, i.e.
 * IRQs must be unmasked. LowPower_Sleep is left byte-for-byte vendor code
 * (it also handles the undocumented PLL/power-plan bits we must not
 * transcribe). RTC_IRQn and the RX-pin GPIO IRQ are the accepted wakes.
 *
 * The check-to-WFI race for ASYNC UART/GPIO wake is NOT closed here: an
 * edge consumed by its ISR during LowPower_Sleep's prologue is lost, and a
 * single NULL byte is not a formal closure (a held BREAK or repeated
 * preamble is -- deferred to the UART sleep-protocol rung, which owns the
 * host contract). The RTC deadline IS race-safe: SLEEP_RTC_MIN_TIME keeps
 * it well ahead of the prologue. This rung's runtime path is inert and its
 * bench experiment is RTC-driven, so the async race is out of scope here.
 * Not RAM-forced: the whole path runs from flash with the HSE up (entry
 * before sleep, wake after Long_Delay settle), matching stock. */
static uint32_t pwr_sleep_until(uint32_t time)
{
    uint32_t time_sleep, time_curr;
    uint32_t irq_state;

    PWR_DIAG_INC16(pwr_sleep_attempt);

    /* Stock early-wake margin, modular (review finding). */
    time = pwr_rtc_sub(time, WAKE_UP_RTC_MAX_TIME);

    /* Mask only for atomic setup + the final wake-source checks. */
    irq_state = pwr_irq_save();

    /* Deep WFI needs interrupts globally acceptable; refuse to sleep from an
     * already-masked caller (review finding). Both real callers -- the TMOS
     * idleCB and the bench hook -- run unmasked. */
    if ((irq_state & 0x08u) == 0u) {
        pwr_irq_restore(irq_state);
        PWR_ABORT(PWR_ABORT_LATE);
        return 3;
    }

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

    /* Plain-WFI mode for the deep path: clear any WFE/SEVONPEND/SLEEPDEEP
     * bits (LowPower_Sleep sets SLEEPDEEP itself). */
    PFIC->SCTLR &= ~((1u << 4) | (1u << 3) | (1u << 2));

    /* Wake-source veto: anything already asserted means the wake edge has
     * passed and an unmasked WFI could miss it. RX line low = a byte is
     * mid-flight; FIFO/ring nonempty; a latched GPIO edge; the RTC trigger
     * flag or a pending RTC/GPIO IRQ. */
    if (
#if PWR_WAKE_PIN_IS_PB
        (R32_PB_PIN & PWR_WAKE_PIN) == 0 || (R16_PB_INT_IF & PWR_WAKE_PIN)
        || PFIC_GetPendingIRQ(GPIO_B_IRQn)
#else
        (R32_PA_PIN & PWR_WAKE_PIN) == 0 || (R16_PA_INT_IF & PWR_WAKE_PIN)
        || PFIC_GetPendingIRQ(GPIO_A_IRQn)
#endif
            || R8_UART1_RFC != 0 || !KeyboardUart_RxQuiet() || pwr_gpio_woke) {
        pwr_gpio_wake_disarm();
        pwr_rtc_trig_disarm();
        pwr_irq_restore(irq_state);
        PWR_ABORT(PWR_ABORT_RXLINE);
        return 3;
    }
    if (RTCTigFlag || (R8_RTC_FLAG_CTRL & RB_RTC_TRIG_FLAG)
            || PFIC_GetPendingIRQ(RTC_IRQn)) {
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

    /* UNMASK, then the stock deep sleep. A wake IRQ arriving from here on
     * either pends before LowPower_Sleep's __WFI (WFI then returns at once)
     * or wakes the WFI normally; on wake its ISR runs and vectors back
     * here. (The narrow prologue race for async wake is the protocol rung's
     * problem, per the header note; RTC wake is race-safe by min-time.) */
    pwr_irq_restore(irq_state);
    pwr_low_power_sleep(RB_PWR_RAM2K | RB_PWR_RAM24K | RB_PWR_EXTEND | RB_XT_PRE_EN);
    irq_state = pwr_irq_save();

    /* Wake reason: the waking interrupt's handler ran at the WFI wake, so
     * the RTC ISR has already set RTCTigFlag; a GPIO wake ran our handler
     * (pwr_gpio_woke). */
    if (RTCTigFlag) {
        PWR_DIAG_INC16(pwr_wake_rtc);        /* our own deadline: no holdoff */
    } else if (pwr_gpio_woke) {
        PWR_DIAG_INC16(pwr_wake_gpio);
        pwr_note_activity();                 /* host woke us: hold off re-sleep */
    } else {
        PWR_ABORT(PWR_ABORT_LATE);           /* spurious prologue return */
    }

    pwr_gpio_wake_disarm();
    pwr_rtc_trig_disarm();
    pwr_irq_restore(irq_state);
    return 0;
}

/*********************************************************************
 * PowerSleep_ExplicitOnce -- host-commanded (A6 54) one-shot deep sleep,
 * GPIO-only wake (power ladder MR6). No RTC deadline: the WWDG is frozen in
 * deep sleep (MR5 bench), so the semantic is "sleep until the host wakes us
 * with an RX edge" -- an RTC backstop would only spend active current after
 * N minutes for nothing. Any UART start bit is a falling edge on the RX
 * pin, so the module is never irrecoverably stranded even if a host omits
 * the NULL preamble (that first frame is lost; a retry wakes it).
 *
 * The check-to-WFI race is closed at the PROTOCOL layer for explicit sleep:
 * the host issued A6 54, so it waits the documented entry-settle before
 * sending any wake byte; the wake edge therefore lands well after the WFI.
 * Returns 0 slept, 3 vetoed. Caller (Power_Service) has already drained TX,
 * flushed the bond, and disconnected. */
uint32_t PowerSleep_ExplicitOnce(void)
{
    uint32_t irq_state;

    PWR_DIAG_INC16(pwr_sleep_attempt);

    irq_state = pwr_irq_save();
    if ((irq_state & 0x08u) == 0u) {       /* deep WFI needs IRQs acceptable */
        pwr_irq_restore(irq_state);
        PWR_ABORT(PWR_ABORT_LATE);
        return 3;
    }

    /* Silence the 200 ms TMR3 heartbeat around the sleep: it is the only
     * always-running periodic IRQ, and a TMR3 edge coincident with the
     * unmasked WFI in LowPower_Sleep's prologue could end the sleep before
     * a GPIO wake (Fsys still runs there; TMR3 itself freezes once deep
     * sleep is entered). Restored after wake (review finding). */
    PFIC_DisableIRQ(TMR3_IRQn);

    pwr_gpio_woke = 0;
    pwr_gpio_wake_arm();
    PFIC->SCTLR &= ~((1u << 4) | (1u << 3) | (1u << 2));   /* plain-WFI mode */

    /* Wake-source veto (GPIO only; no RTC armed): an already-asserted edge
     * or queued byte means the wake could be missed. */
    if (
#if PWR_WAKE_PIN_IS_PB
        (R32_PB_PIN & PWR_WAKE_PIN) == 0 || (R16_PB_INT_IF & PWR_WAKE_PIN)
        || PFIC_GetPendingIRQ(GPIO_B_IRQn)
#else
        (R32_PA_PIN & PWR_WAKE_PIN) == 0 || (R16_PA_INT_IF & PWR_WAKE_PIN)
        || PFIC_GetPendingIRQ(GPIO_A_IRQn)
#endif
            || R8_UART1_RFC != 0 || !KeyboardUart_RxQuiet() || pwr_gpio_woke) {
        pwr_gpio_wake_disarm();
        PFIC_EnableIRQ(TMR3_IRQn);
        pwr_irq_restore(irq_state);
        PWR_ABORT(PWR_ABORT_RXLINE);
        return 3;
    }

#if WATCHDOG_ENABLE
    WWDG_SetCounter(0);
#endif
    PWR_DIAG_INC16(pwr_sleep_entered);

    pwr_irq_restore(irq_state);
    pwr_low_power_sleep(RB_PWR_RAM2K | RB_PWR_RAM24K | RB_PWR_EXTEND | RB_XT_PRE_EN);
    irq_state = pwr_irq_save();

    /* Classify BEFORE disarm: pwr_gpio_wake_disarm() itself sets pwr_gpio_woke
     * from a late pending edge, which would mask an anomalous wake (review).
     * A clean RX-edge wake ran our handler and returns 0; any other return
     * (a spurious prologue wake) is reported as 3 so the caller can tell. */
    uint32_t rc;
    if (pwr_gpio_woke) {
        PWR_DIAG_INC16(pwr_wake_gpio);
        pwr_note_activity();                 /* host woke us: hold off re-sleep */
        rc = 0;
    } else {
        PWR_ABORT(PWR_ABORT_LATE);
        rc = 3;
    }
    pwr_gpio_wake_disarm();
    PFIC_EnableIRQ(TMR3_IRQn);
    pwr_irq_restore(irq_state);
    return rc;
}

/*********************************************************************
 * Auto-sleep controls (MR7), mirrored from the sleep-protocol reducer. */
void PowerSleep_SetAutosleep(uint8_t on)
{
    if (on && !pwr_autosleep) {
        pwr_note_activity();   /* arming counts as activity: no instant sleep */
    }
    pwr_autosleep = on ? 1u : 0u;
}

void PowerSleep_NoteActivity(void)
{
    pwr_note_activity();
}

/*********************************************************************
 * PowerSleep_IdleAutosleep -- main-loop autonomous site for RF IDLE
 * (MR7). Sleeps until the host's RX edge (GPIO-only, no deadline, same
 * one-shot core as the explicit A6 54 path). Returns 0 if it slept and woke,
 * 3 if not applicable / vetoed. The caller has already checked RF IDLE, no
 * OpenBoot entry, no pending explicit sleep, and RX quiet. */
uint32_t PowerSleep_IdleAutosleep(void)
{
    if (!pwr_autosleep || !pwr_holdoff_expired()) {
        return 3;
    }
    if (!KeyboardUart_TxIdle()) {   /* never sleep with a reply still shifting */
        PWR_ABORT(PWR_ABORT_TX);
        return 3;
    }
    return PowerSleep_ExplicitOnce();
}

/*********************************************************************
 * CH59x_LowPower -- the TMOS idle callback (MCU.c wires it under
 * HAL_SLEEP=TRUE). `time` is the next TMOS deadline as an absolute RTC
 * tick. Autonomous site for the BONDED RECONNECT SEARCH only (MR7): between
 * beacons, once the duty cycle has shut the receiver, TMOS hands us the
 * ~17 ms to the next beacon and we deep-sleep it out (RTC + GPIO wake). RF
 * IDLE is deliberately refused here -- the main loop owns it -- so nothing
 * depends on what deadline TMOS passes when it has nothing scheduled. */
__HIGH_CODE
uint32_t CH59x_LowPower(uint32_t time)
{
    if (!pwr_autosleep) {
        return 3;   /* not armed: behaves exactly as MR6 */
    }
    if (RF_GetState() == RF_STATE_IDLE) {
        return 3;   /* main-loop site owns IDLE (GPIO-only, no deadline) */
    }
    if (!RF_CanDeepSleep()) {
        PWR_ABORT(PWR_ABORT_RF);
        return 3;
    }
    if (!pwr_holdoff_expired()) {
        return 3;   /* host recently spoke: stay awake for its next frame */
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
 * per UART request (A6 54 -> PowerSleep_BenchRequest), main-loop context.
 * Bench-answered on hardware 2026-09-03: RTC deep-sleep wake works, the
 * floor is below the meter's ~10 uA resolution, and the WWDG is frozen
 * during deep sleep (a 5 s sleep with the watchdog on held at floor and did
 * not reset at the 559 ms window). Run WATCHDOG_ENABLE=0 vs on to compare. */
void PowerSleep_BenchRequest(void)
{
    pwr_bench_phase = 0x01u;
}

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
