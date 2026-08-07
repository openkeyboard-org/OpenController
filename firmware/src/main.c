/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 *
 * OpenController keyboard wireless-module firmware for the WCH CH592F.
 *
 * This is intentionally a keyboard-module application, not a USB dongle:
 * UART1 is remapped to PB12/PB13 and speaks the wireless-module binary
 * command/status protocol used by the keyboard's host MCU.
 */

#include "CONFIG.h"
#include "HAL.h"
#include "keyboard_uart.h"
#include "rf_task.h"
#include "openboot_app.h"

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if (defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0xbd, 0xc3, 0xab, 0x10, 0x53, 0x5c};
#endif

static uint8_t transport_is_2g4;

/* Latched by the A6 81 UART command, acted on by OpenBoot_Service(). */
static volatile uint8_t openboot_entry_pending;

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
 *   FLUSH_BOND — post a pending deferred bond save, then let
 *                TMOS_SystemProcess() run it on the next pass (an A6 81
 *                arriving right after a fresh pair must not lose the bond:
 *                RF_Disconnect() clears the pending flag);
 *   QUIESCE    — RF_Disconnect(): TMR0 stopped, RF tasks stopped, RF_Shut;
 *   DRAIN      — wait for the frame's 61 0D 0A ACK to physically leave the
 *                UART (bounded ~20 ms wall clock: a host not draining must
 *                not block the update);
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
        svc_state = 1;
        return;                 /* TMOS_SystemProcess runs the save next pass */
    case 1:
        RF_Disconnect();
        svc_start = SYS_GetSysTickCnt();
        svc_state = 2;
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

__HIGH_CODE
__attribute__((noinline))
void Main_Circulation(void)
{
    while (1) {
        TMOS_SystemProcess();
        RF_ConnectedTick();
        KeyboardUart_Poll();
        OpenBoot_Service();
        WATCHDOG_FEED();
    }
}

/* Boot-phase sentinel at 0x20005800, continuing the startup's 0xC0..0xC5
 * series (see startup_CH592_phased.S): 0xA0.. marks main()'s init phases so
 * a wedge is SWD-attributable without a debugger. */
#define BOOT_PHASE(x)  (*(volatile uint8_t *)0x20005800 = (uint8_t)(x))

int main(void)
{
    BOOT_PHASE(0xA0);
    SetSysClock(CLK_SOURCE_PLL_60MHz);
    BOOT_PHASE(0xA1);

    KeyboardUart_Init();
    KeyboardUart_SetFrameCallback(handle_uart_frame);
    BOOT_PHASE(0xA2);

    /* Normalize OpenBoot-inherited SysTick state. The bootloader leaves
     * SysTick free-running (its idle-timeout clock); CH59x_BLEInit's
     * SysTick_Config enables the SysTick IRQ one line before PFIC disables
     * it, and a pending count-flag inherited from the bootloader fires in
     * that window, vectoring into the startup's weak infinite-loop handler
     * (bench-diagnosed: boot phase parked at the pre-BLEInit marker). Stop
     * the counter and drop any pending state before the HAL touches it. */
    SysTick->CTLR = 0;
    SysTick->SR = 0;
    PFIC_ClearPendingIRQ(SysTick_IRQn);

    CH59x_BLEInit();
    BOOT_PHASE(0xA3);
    HAL_Init();
    BOOT_PHASE(0xA4);
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
    if (RF_HasBond()) {
        transport_is_2g4 = 1;
        KeyboardUart_SendStatus(0x34);
        KeyboardUart_SendStatus(0x35);
    }
    watchdog_init();
    BOOT_PHASE(0xA7);
    Main_Circulation();
}
