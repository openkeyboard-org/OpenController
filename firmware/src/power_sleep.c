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
 * link). EVERY deep sleep runs inside the TMOS idle callback CH59x_LowPower
 * and returns 0, so the BLE library re-initialises its baseband on the wake
 * (a sleep taken behind TMOS's back hangs the next RF op -> WWDG reset;
 * bench-proven 2026-09-04). Main-loop code only REQUESTS a sleep:
 * PowerSleep_RequestExplicit() for the host A6 54 (after Power_Service has
 * disconnected the link), and the pwr_autosleep flag for A6 57 auto-sleep.
 * The idle callback then sleeps in RF IDLE (host wakes it via the RX edge)
 * and in the radio-off slices of a bonded reconnect search, always honouring
 * TMOS's next deadline so library housekeeping timers are not starved.
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
#ifndef KBD_UART_DIAG_DUMP
#define KBD_UART_DIAG_DUMP RF_DIAG_COUNTERS
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
/* Main-loop liveness: incremented once per Main_Circulation pass (main.c).
 * Preserved across a WWDG reset like the counters above -- a stalled loop
 * shows as a frozen value in the post-crash dump. */
volatile uint16_t pwr_loop_passes __attribute__((section(".diag_safe.power")));
/* Last main-loop / sleep-path stage reached (forensics build); see main.c. */
volatile uint8_t  pwr_loop_stage  __attribute__((section(".diag_safe.power")));
/* After a WATCHDOG-caused boot the forensic counters are FROZEN (not
 * incremented) until the host zeroes them (A6 72), so the post-crash UART dump
 * shows the pre-crash values, not the reboot's activity. */
volatile uint8_t  pwr_forensics_frozen;
#ifndef KBD_IDLECB_FORENSICS
#define KBD_IDLECB_FORENSICS 0
#endif
#if KBD_IDLECB_FORENSICS
/* Forensics build only (EXTRA_CFLAGS=-DKBD_IDLECB_FORENSICS=1): repurpose two
 * dumped counters -- pwr_wake_rtc := idleCB call count, pwr_last_abort_reason
 * := last idleCB gate reached (1 off, 2 rf-idle, 3 rf-busy, 4 holdoff,
 * 5 openboot, 6 tx, 7 rx, 8 sleep-called). Never commit enabled. */
#define IDLECB_GATE(n)  do { if (!pwr_forensics_frozen) { pwr_last_abort_reason = (n); pwr_loop_stage = 0x10u | (n); } } while (0)
#define IDLECB_CALL()   do { if (!pwr_forensics_frozen) { pwr_wake_rtc++; } } while (0)
#define PWR_STAGE(n)    do { if (!pwr_forensics_frozen) { pwr_loop_stage = (n); } } while (0)
#else
#define IDLECB_GATE(n)  do { } while (0)
#define IDLECB_CALL()   do { } while (0)
#define PWR_STAGE(n)    do { } while (0)
#endif
/* Frozen after a WATCHDOG boot until the first UART dump (see HAL_SleepInit /
 * DiagDump_Send): the pre-crash values are the forensic record. */
extern volatile uint8_t pwr_forensics_frozen;
#define PWR_DIAG_INC16(x)   do { if (!pwr_forensics_frozen) { (x)++; } } while (0)
#define PWR_ABORT(r)        do { if (!pwr_forensics_frozen) { pwr_last_abort_reason = (r); \
                                 pwr_sleep_aborted++; } } while (0)
#else
#define PWR_DIAG_INC16(x)   do { } while (0)
#define PWR_ABORT(r)        do { } while (0)
/* No counters => no forensics either. */
#define IDLECB_GATE(n)      do { } while (0)
#define IDLECB_CALL()       do { } while (0)
#define PWR_STAGE(n)        do { } while (0)
#endif

/* Always-present symbol so a KBD_IDLECB_FORENSICS build links even with
 * RF_DIAG_COUNTERS=0 (the stage byte then has no backing store; PWR_STAGE is a
 * no-op in that profile). Review finding. */
void PowerSleep_Stage(uint8_t n) { PWR_STAGE(n); }

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

/* Deferred sleep requests, consumed by the TMOS idle callback. EVERY deep
 * sleep is performed inside CH59x_LowPower and reported to TMOS by returning
 * 0: the BLE library re-initialises its baseband/LLE on that return, and a
 * deep sleep taken behind its back (as the first MR6/MR7 code did from the
 * main loop) leaves the next RF operation hanging inside TMOS_SystemProcess
 * until the WWDG resets the part (bench, 2026-09-04: reproducible 100%). */
static volatile uint8_t pwr_explicit_request;   /* A6 54, after quiesce */
#if KBD_SLEEP_BENCH_HOOK
static volatile uint8_t  pwr_bench_request;
static uint32_t pwr_bench_deadline;
#endif

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
 * is the ABSOLUTE RTC tick to wake at. Return contract (stock-compatible; this is the TMOS idleCB):
 * 0 = LowPower_Sleep was ENTERED and returned -- by the RTC deadline, a GPIO
 * wake, or a spurious prologue return (pwr_wake_rtc / pwr_wake_gpio /
 * pwr_last_abort_reason say which); TMOS uses 0 to run its LLE/BB wake
 * re-init, wanted after ANY entry, so a spurious return must still report 0
 * (review). 2 = window rejected, 3 = vetoed before entry (nothing entered).
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
    PWR_STAGE(0x21);

    /* UNMASK, then the stock deep sleep. A wake IRQ arriving from here on
     * either pends before LowPower_Sleep's __WFI (WFI then returns at once)
     * or wakes the WFI normally; on wake its ISR runs and vectors back
     * here. (The narrow prologue race for async wake is the protocol rung's
     * problem, per the header note; RTC wake is race-safe by min-time.) */
    pwr_irq_restore(irq_state);
    pwr_low_power_sleep(RB_PWR_RAM2K | RB_PWR_RAM24K | RB_PWR_EXTEND | RB_XT_PRE_EN);
    irq_state = pwr_irq_save();
    PWR_STAGE(0x22);

    /* Wake reason: the waking interrupt's handler ran at the WFI wake, so
     * the RTC ISR has already set RTCTigFlag; a GPIO wake ran our handler
     * (pwr_gpio_woke). */
    if (RTCTigFlag) {
        PWR_DIAG_INC16(pwr_wake_rtc);        /* our own deadline: no holdoff */
    } else if (pwr_gpio_woke) {
        PWR_DIAG_INC16(pwr_wake_gpio);
        pwr_note_activity();                 /* host woke us: hold off re-sleep */
    } else {
        /* Spurious prologue return: counted for diag, but LowPower_Sleep WAS
         * entered, so the return stays 0 per the contract above. */
        PWR_ABORT(PWR_ABORT_LATE);
    }

    pwr_gpio_wake_disarm();
    pwr_rtc_trig_disarm();
    pwr_irq_restore(irq_state);
    return 0;
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
 * PowerSleep_RequestExplicit -- Power_Service has quiesced (TX drained,
 * bond flushed, RF disconnected); the idle callback performs the sleep on
 * TMOS's next idle pass and reports it to TMOS. Idempotent. The request
 * stays armed across RTC-deadline wakes (TMOS housekeeping such as the
 * 120 s HAL calibration event) and is cleared only by a HOST wake (GPIO) or
 * by PowerSleep_CancelExplicit(). */
void PowerSleep_RequestExplicit(void)
{
    pwr_explicit_request = 1;
}

/* Any state-changing host command (transport select, pair, unpair, OTA,
 * HID/name) withdraws an armed explicit request -- independently of the
 * reducer's own sleep_pending, which Power_Service has already consumed by
 * the time the request is armed (review finding: a stale request would
 * otherwise fire as a surprise sleep on the next idle). */
void PowerSleep_CancelExplicit(void)
{
    pwr_explicit_request = 0;
}

uint8_t PowerSleep_SleepPending(void)
{
    return pwr_explicit_request;
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
    IDLECB_CALL();

#if KBD_SLEEP_BENCH_HOOK
    if (pwr_bench_request) {
        pwr_bench_request = 0;
        {
            uint32_t rc = pwr_sleep_until(pwr_bench_deadline);
            pwr_bench_phase = (rc == 0) ? 0x30u : (rc == 2) ? 0x42u : 0x43u;
            return rc;
        }
    }
#endif

    /* Every sleep below honours `time`, TMOS's next deadline: the HAL keeps
     * housekeeping timers (e.g. a 120 s calibration event) that an indefinite
     * sleep would starve (review finding). Sleeping to the deadline and
     * re-entering costs a few microamps; the host still wakes us any time via
     * the RX edge. A wake is ALWAYS reported to TMOS as 0 once LowPower_Sleep
     * was entered -- TMOS re-inits its baseband only on exactly 0. */

    /* Explicit A6 54 sleep, requested by Power_Service after the quiesce.
     * Vetoed (left armed) unless RF IDLE / no OTA / UART quiet; consumed only
     * by a HOST (GPIO) wake, so an RTC-deadline wake re-sleeps. */
    if (pwr_explicit_request) {
        if (RF_GetState() != RF_STATE_IDLE || OpenBoot_EntryPending()
                || !KeyboardUart_TxIdle() || !KeyboardUart_RxQuiet()) {
            IDLECB_GATE(9);
            return 3;
        }
        IDLECB_GATE(8);
        {
            uint32_t rc = pwr_sleep_until(time);
            if (rc == 0 && pwr_gpio_woke) {
                pwr_explicit_request = 0;   /* the host is back: request done */
            }
            return rc;
        }
    }

    if (!pwr_autosleep) {
        IDLECB_GATE(1);
        return 3;   /* not armed: behaves exactly as MR6 */
    }
    if (OpenBoot_EntryPending()) {
        IDLECB_GATE(5);
        PWR_ABORT(PWR_ABORT_OPENBOOT);
        return 3;
    }
    if (!pwr_holdoff_expired()) {
        IDLECB_GATE(4);
        return 3;   /* host recently spoke: stay awake for its next frame */
    }
    if (!KeyboardUart_TxIdle()) {
        IDLECB_GATE(6);
        PWR_ABORT(PWR_ABORT_TX);
        return 3;
    }
    if (!KeyboardUart_RxQuiet()) {
        IDLECB_GATE(7);
        PWR_ABORT(PWR_ABORT_RX);
        return 3;
    }
    if (RF_GetState() != RF_STATE_IDLE && !RF_CanDeepSleep()) {
        IDLECB_GATE(3);
        PWR_ABORT(PWR_ABORT_RF);
        return 3;   /* CONNECTED, fresh pairing, or the RX window is open */
    }
    /* Armed IDLE (sleep to the TMOS deadline, host wakes us any time) or the
     * armed bonded-search radio-off slice (RTC-bounded by the beacon). */
    IDLECB_GATE(RF_GetState() == RF_STATE_IDLE ? 2 : 8);
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
    /* .diag_safe.power is NOLOAD. Zero on every boot EXCEPT a watchdog reset:
     * after a WWDG reset the pre-crash values are the forensic record (the
     * UART diag dump reads them), so keep them. R8_RESET_STATUS is stashed at
     * 0x20005801 by the phased startup (see src/fault_handler.S). */
#if KBD_UART_DIAG_DUMP
    if (((*(volatile uint8_t *)0x20005801u) & 0x07u) == 0x02u /* RST_FLAG_WTR */) {
        pwr_forensics_frozen = 1;   /* keep the pre-crash record until the dump reads it */
    } else
#endif
    {
    pwr_sleep_attempt = 0;
    pwr_sleep_entered = 0;
    pwr_sleep_aborted = 0;
    pwr_wake_gpio = 0;
    pwr_wake_rtc = 0;
    pwr_last_abort_reason = 0;
    pwr_loop_passes = 0;
    pwr_loop_stage = 0;
    }
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
        /* Performed from the idle callback like every other deep sleep (see
         * CH59x_LowPower); the RTC-bounded 5 s window is armed here. */
        pwr_bench_deadline = pwr_rtc_add(RTC_GetCycle32k(), 5u * FREQ_RTC);
        pwr_bench_request = 1;
        pwr_bench_phase = 0x20u;   /* entering -- survives a WWDG reset */
    }
}
#endif
