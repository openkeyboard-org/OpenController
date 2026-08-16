/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 */
#include "CONFIG.h"
#include "HAL.h"
#include "ISP592.h"
#include "keyboard_uart.h"
#include "rf_task.h"
#if KBD_RF_CRYPT
#include "kbd_rf_crypt.h"
#endif

#define RF_EVT_START              0x0001
#define RF_EVT_RX_RESTART         0x0002
#define RF_EVT_PAIR_BCAST         0x0004
#define RF_EVT_PAIR_TIMEOUT       0x0008
#define RF_EVT_ENTER_CONNECTED    0x0010
#define RF_EVT_RESPOND            0x0020   /* connected poll-response TX (main loop) */
#define RF_EVT_NOTIFY_LED         0x0040
#define RF_EVT_SAVE_BOND          0x0100   /* deferred DataFlash write, not from RF ISR */
#if KBD_RF_CRYPT
#define RF_EVT_CRYPT_SESSION      0x0200   /* verify+adopt a received session frame */
#define RF_EVT_CRYPT_ARM          0x0400   /* pre-seal the next encrypted uplink frame */
#endif

/* Stock-interop H1/H2 discriminator: skip the first N connected-poll responses
 * (keep listening/tracking, no uplink TX). If the keyboard then catches several
 * MORE stock polls before teardown, the running RX/track is fine and it is our
 * RESPONSE that trips the stock dongle's fast supervision teardown (H1). Needs
 * RF_DIAG_COUNTERS (uses the .diag_safe ll_resp_suppressed counter). */
#ifndef STOCK_SUPPRESS_RESPONSES
#define STOCK_SUPPRESS_RESPONSES 0
#endif
/* H1/H2 probe: after the first caught poll, stay parked on that channel (never
 * advance the time-based hop). If the dongle is still polling, the parked
 * keyboard keeps catching it (=> our hop tracking is the fault); if not, the
 * dongle stopped (=> it tore the link down). */
#ifndef STOCK_PARK_AFTER_CATCH
#define STOCK_PARK_AFTER_CATCH 0
#endif
/* Bench probe: transmit the poll response directly in the TMR0 turnaround ISR
 * instead of deferring to the main-loop RF_EVT_RESPOND event. The direct ISR
 * path did not fix stock reconnect; the validated default is main-loop TX after
 * the 6000-count turnaround below. */
#ifndef STOCK_ISR_FAST_RESPONSE
#define STOCK_ISR_FAST_RESPONSE 0
#endif
/* Fast-response fix (matches stock firmwareB.bin 0x20000f06): skip the redundant
 * RF_SetChannel before the response TX -- the radio is already on the poll channel;
 * re-setting it costs a ~150us PLL relock that misses the stock dongle's RX window. */
#ifndef STOCK_RESP_NO_SETCH
#define STOCK_RESP_NO_SETCH 1
#endif
/* Acquisition coarse-align probe: on the FIRST connected poll
 * caught while parked (provisional), SNAP hop_anchor to that poll's measured
 * start instead of a ±1 servo nudge -- the ±1 servo can't lock a large initial
 * phase error from a single parked catch, so the keyboard misses subsequent
 * polls off the real stock dongle's grid. Keeps the ±1 servo for later polls. */
#ifndef STOCK_ACQ_SNAP
#define STOCK_ACQ_SNAP 0
#endif
/* Gap-free acquisition probe: while provisional, keep the radio continuously
 * parked on the seed/catch channel instead of RF_Shut+RF_Rx re-arming every
 * interval. Pair with STOCK_PARK_AFTER_CATCH=1 to stay parked after the first
 * catch and prove whether periodic re-arm gaps were hiding later stock polls. */
#ifndef STOCK_CONTINUOUS_PARK_ACQ
#define STOCK_CONTINUOUS_PARK_ACQ 0
#endif
/* Bench probe: some stock-dongle reconnect state reports one interval in the
 * LEN-15 payload while its measured connected-poll cadence is another. Nonzero
 * overrides the parsed connected interval after sanity-checking the packet. */
#ifndef STOCK_FORCE_CONN_INTERVAL
#define STOCK_FORCE_CONN_INTERVAL 0
#endif
/* Stock-interop fix: the pair-ACK type tag is the previous connected-hop seed.
 * The dongle's first connected poll one interval later is therefore seed+1.
 * Keep this configurable only for bench regressions against old hypotheses. */
#ifndef STOCK_CONNECT_IDX_BIAS
#define STOCK_CONNECT_IDX_BIAS 1
#endif
#define RF_EVT_DISCONNECT         0x0080

#define RF_DEFAULT_ACCESS_ADDR    0x71764126u
#define RF_CRC_INIT               0x555555u
#define RF_LLE_MODE               (LLE_MODE_BASIC | LLE_WHITENING_ON | LLE_MODE_PHY_2M)
#define RF_RX_MAX_LEN             70

#define RF_PAIR_INTERVAL          28u
#define RF_PAIR_TIMEOUT_VALUE     600u
#define RF_PAIR_BCAST_TICKS       32u      /* 20 ms at 625 us/tick */
#define RF_PAIR_WINDOW_TICKS      8480u    /* stock pair window is about 5.3 s */
#define RF_PAIR_DWELL_BCASTS      12u
#define RF_CONNECTED_TIMEOUT_TICKS 5000u   /* ~3.1 s; FIXED connection supervision --
                                            * the dongle's advertised timeout field is
                                            * intentionally not honored (stock scales it
                                            * by an unproven factor; this is validated) */

#define RF_BOND_EEPROM_OFF        0x4000u  /* DataFlash 0x74000: stock keyboard 2.4G bond page */
#define RF_BOND_EEPROM_ERASE_LEN  256u
#define RF_BOND_MAGIC             0x3244424bu  /* "KBD2" little-endian */
#if KBD_RF_CRYPT
/* v2 appends the negotiated flags and the 16-byte link key. The version bump is
 * deliberate and there is NO migration: a v1 record predates any notion of a
 * key, so it is invalidated and the unit re-pairs, which is exactly the right
 * semantics when encryption state enters the bond. The record grows from 24 to
 * 40 bytes inside an erase page of 256, and the checksum already covers
 * sizeof-4, so nothing else has to move. */
#define RF_BOND_VERSION           2u
#define RF_BOND_FLAG_ENC_CAPABLE  0x01u  /* peer negotiated encryption at pairing */
#define RF_BOND_FLAG_ENC_KEY      0x02u  /* link_key holds a provisioned key      */
#else
#define RF_BOND_VERSION           1u
#endif

#define R32_RTC_CNT_32K_ADDR      0x40001038u
#define SYSTICK_CNT_ADDR          0xE000F008u
#define RTC32K_WRAP               0xa8c00000u   /* RTC32K counter modulus */
#define RF_POLL_BACKDATE_BASE     12u           /* RX latency: a caught poll's start is
                                                 * now-(BASE+(len>>3)) -- LEN 1/3 -> 12,
                                                 * LEN 10/15 -> 13 (the inline servo) */
#define RF_HOP_ANCHOR_BACKDATE    (RF_POLL_BACKDATE_BASE + 1u)  /* seed/re-seed (LEN>=8) */
#define RF_HOP_DESYNC_INTERVALS   64u           /* gap (in intervals) treated as desync */

#ifndef KBD_MAC_0
#define KBD_MAC_0 0xbd
#define KBD_MAC_1 0xc3
#define KBD_MAC_2 0xab
#define KBD_MAC_3 0x10
#define KBD_MAC_4 0x53
#define KBD_MAC_5 0x5c
#endif

#ifndef STOCK_SEED_BOND
#define STOCK_SEED_BOND 0
#endif
#ifndef STOCK_SEED_SESSION_AA
#define STOCK_SEED_SESSION_AA 0x00000000u
#endif
#ifndef STOCK_SEED_TYPE_TAG
#define STOCK_SEED_TYPE_TAG 0u
#endif
#ifndef STOCK_SEED_DONGLE_MAC_0
#define STOCK_SEED_DONGLE_MAC_0 0x00
#define STOCK_SEED_DONGLE_MAC_1 0x00
#define STOCK_SEED_DONGLE_MAC_2 0x00
#define STOCK_SEED_DONGLE_MAC_3 0x00
#define STOCK_SEED_DONGLE_MAC_4 0x00
#define STOCK_SEED_DONGLE_MAC_5 0x00
#endif

static const uint8_t keyboard_mac[6] = {
    KBD_MAC_0, KBD_MAC_1, KBD_MAC_2, KBD_MAC_3, KBD_MAC_4, KBD_MAC_5
};
#define NUM_PAIR_CHANNELS  3u
#define NUM_DATA_CHANNELS  5u
#define RF_HOP_MAX_STEP    NUM_DATA_CHANNELS  /* clamp a stale advance to one full hop cycle */
static const uint8_t pair_channels[NUM_PAIR_CHANNELS] = {8, 17, 26};
static const uint8_t data_channels[NUM_DATA_CHANNELS] = {4, 13, 20, 28, 33};

static uint8_t rf_taskID;
static uint8_t rf_state;
static int8_t rf_rssi;
static uint32_t rf_access_addr;
static uint8_t rf_channel;
static uint16_t rf_conn_interval;

static uint8_t has_bond;
static uint8_t stored_dongle_mac[6];
static uint32_t stored_session_aa;
static uint8_t stored_type_tag;
#if KBD_RF_CRYPT
static uint8_t stored_bond_flags;
static uint8_t stored_link_key[KBD_CRYPT_KEY_BYTES];
#endif

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type_tag;
#if KBD_RF_CRYPT
    uint8_t  flags;         /* RF_BOND_FLAG_ENC_*; was the low half of reserved0 */
    uint8_t  reserved0;
#else
    uint16_t reserved0;
#endif
    uint32_t session_aa;
    uint8_t  dongle_mac[6];
    uint16_t reserved1;
#if KBD_RF_CRYPT
    uint8_t  link_key[KBD_CRYPT_KEY_BYTES];  /* valid iff flags & ENC_KEY */
#endif
    uint32_t checksum;
} rf_bond_record_t;

#if KBD_RF_CRYPT
/* Encryption is NEGOTIATED, never assumed: it goes active only for a peer that
 * advertised the capability at pairing AND a key that has been provisioned.
 * Mirrors the receiver's bond_enc_active(). */
static uint8_t rf_bond_enc_active(void)
{
    return (stored_bond_flags & (RF_BOND_FLAG_ENC_CAPABLE | RF_BOND_FLAG_ENC_KEY))
           == (RF_BOND_FLAG_ENC_CAPABLE | RF_BOND_FLAG_ENC_KEY);
}
#endif

static uint8_t pair_bcast_count;
static uint8_t pair_payload[10];

static uint8_t pending_type_tag;
static uint16_t pending_interval;
static uint32_t pending_session_aa;

static uint8_t hid_report[8];
#if KBD_RF_CRYPT
/* An encrypted boot-keyboard frame is 22 bytes on air, so the response buffer
 * has to hold one. Sized only in encrypted builds so a plaintext image keeps
 * its exact .bss layout. */
static uint8_t tx_payload[KBD_CRYPT_MAX_FRAME];
/* Session frame copied out of the RX ISR, verified in task context. */
static volatile uint8_t crypt_session_rx[KBD_CRYPT_LEN_SESSION];
static volatile uint8_t crypt_session_rx_pending;
static volatile uint8_t crypt_polls_since_auth;
static volatile uint8_t crypt_keepalive_due;

/* Seal the next uplink frame, and act on a failure instead of discarding it.
 *
 * Neither failure that can reach here is retryable. An engine fault is a fatal
 * fault of the radio path (hal_aes.h says so explicitly), and counter
 * exhaustion needs a re-key, not another attempt. Dropping the session makes
 * the link fail CLOSED in both cases: with no session there is no sealed frame,
 * and rf_do_response_tx then sends bare acks rather than plaintext. Discarding
 * the status instead left the keyboard looking healthy while it had nothing to
 * send, until the receiver's silence guard dropped the link. */
static void rf_crypt_arm(uint8_t ctrl_hint)
{
    kbd_crypt_status_t st;
#if KBD_CRYPT_BENCH_KEY
    /* Bench: re-derive the tag of the frame just transmitted, from its own
     * bytes, before the engine is reused for the next seal. Catches a
     * corrupted seal at the source (selfck_bad) with the receiver out of the
     * loop entirely. */
    kbd_crypt_bench_verify_pending();
#endif
    st = kbd_crypt_seal_begin(ctrl_hint, KBD_CRYPT_TAG_BOOT_KBD,
                              hid_report, sizeof(hid_report));
    if (st == KBD_CRYPT_FAULT_ENGINE || st == KBD_CRYPT_EXHAUSTED) {
        kbd_crypt_end_session();
    }
}
#else
static uint8_t tx_payload[10];
#endif
static uint8_t tx_ctrl;
static uint8_t prev_data_idx;     /* current connected data-channel index */
static volatile uint8_t pending_led;
static volatile uint8_t last_led_sent;
static volatile uint8_t hid_resend;  /* >0: send LEN=10 HID report this many more polls */

/* Connected-mode time-based hop (mirrors stock firmwareB.bin 0x20000E06/0x20000D60).
 * hop_anchor (like tx_ctrl) is written by BOTH the RX ISR (hop_servo / ctrl-ARQ) and the
 * main loop, yet is deliberately NOT volatile: every main-loop access is wrapped in
 * rf_irq_save/rf_irq_restore (IRQs off) so the ISR can't interleave a partial update, and
 * the "memory" clobber bars stale caching across the section. */
static uint32_t hop_anchor;             /* RTC32K phase anchor (gp+0x78 equivalent) */
static volatile uint16_t since_rx;      /* hop slots since the last caught poll */
static uint32_t pending_anchor;         /* anchor latched at the 2nd 15-byte (rx-13) */

/* Response-slot latch (Codex): the response must TX on the channel the poll was
 * received on, not the (possibly already-advanced) hop channel, and the hop must
 * not retune the radio while a response is pending. */
static volatile uint8_t  response_channel;
static volatile uint8_t  response_ctrl;
static volatile uint8_t  response_pending;
static volatile uint32_t response_armed_rtc;   /* RTC32K when response_pending was set */
/* Set at (re)connect: PARK on the seed channel (re-arm RX every interval, don't
 * advance the index) until the first poll is caught -- a swept keyboard never
 * meets the swept dongle, so wait for the dongle to sweep onto our parked
 * channel, then the first caught poll snaps the phase and we resume tracking.
 * This is what makes a bonded reconnect lock in (its 15-byte seed mis-aligns
 * the sweep, and with zero catches there is nothing to snap to). */
static volatile uint8_t  provisional;
static volatile uint8_t  bond_save_pending;

/* Set by the RX ISR when a connected-mode poll arrives; the main loop
 * (RF_ConnectedTick) re-arms the supervision timer from it. tmos_start_task /
 * tmos_stop_task mutate the TMOS timer linked list, and our BB status callback is
 * a hardware fast-vectored (VTF) interrupt the lib's TMOS critical section does
 * not mask -- so mutating the list from the ISR would race the main-loop walk
 * (tmos_proces_system_time). We keep list mutation in main-loop context only as a
 * correctness/ISR-safety measure. (NB: the v1.4.2 "TMOS HardFault" this originally
 * chased turned out to be a debugger-perturbation artifact, not an operational
 * fault -- see docs/TMOS_REVIEW.md -- but deferring ISR list mutation is good practice
 * regardless and is retained.) */
static volatile uint8_t  supervision_kick;

#define HID_RESEND_COUNT  6u   /* resend a changed report on this many polls */

/* TMR0 one-shot post-poll turnaround. Stock firmwareB.bin FUN_ram_00005abe arms
 * 300 TMR0 counts (~5 us @ 60 MHz), but this main-loop deferred TX path needs a
 * longer empirically validated delay for the production stock dongle's RX turn:
 * 6000 counts (~100 us) keeps replies inside the stock post-poll window. */
#ifndef RF_TURNAROUND_COUNT
#define RF_TURNAROUND_COUNT  6000u
#endif

/* SWD-readable diagnostic counters (.diag_safe, NOLOAD -- no flash cost). Gated
 * behind RF_DIAG_COUNTERS (default 1; build a release image with
 * `make RF_DIAG_COUNTERS=0`) so the always-on read-modify-writes -- including in
 * the RX-ISR hot path -- and their SRAM cost vanish when not benching. Use the
 * RF_DIAG_*() macros so a disabled build never drops a wrapped side effect. These are
 * intentionally external symbols (lowercase, despite the file's "lowercase => static"
 * convention) so SWD and the .map can resolve them by name. */
#ifndef RF_DIAG_COUNTERS
#define RF_DIAG_COUNTERS 1
#endif
#if RF_DIAG_COUNTERS
#if KBD_RF_CRYPT
/* Session-adoption trace. Not gated on RF_DIAG_COUNTERS: an encrypted link that
 * never adopts a session is silently dead -- it sends only bare acks, which look
 * exactly like a healthy idle link until the receiver gives up -- and these
 * three counters are the difference between diagnosing that in one bench run
 * and guessing. 12 bytes of the .diag_safe window. */
volatile uint32_t kbd_crypt_sess_rx  __attribute__((section(".diag_safe")));  /* LEN-14 announces seen by the ISR */
volatile uint32_t kbd_crypt_sess_ok  __attribute__((section(".diag_safe")));  /* verified and adopted */
volatile uint32_t kbd_crypt_sess_bad __attribute__((section(".diag_safe")));  /* verify rejected */
volatile uint32_t kbd_crypt_tx_sealed __attribute__((section(".diag_safe")));  /* sealed frames handed to RF_Tx */
volatile uint32_t kbd_crypt_seal_miss __attribute__((section(".diag_safe")));  /* wanted one, none was ready */
#endif
volatile uint32_t rf_cb_count[6] __attribute__((section(".diag_safe")));
volatile uint32_t rf_pair_bcast_count __attribute__((section(".diag_safe")));
volatile uint32_t rf_connected_tx_count __attribute__((section(".diag_safe")));
volatile uint32_t rf_valid_rx_count __attribute__((section(".diag_safe")));
volatile uint8_t rf_last_config_status __attribute__((section(".diag_safe")));
volatile uint8_t rf_last_rx_status __attribute__((section(".diag_safe")));
volatile uint8_t rf_last_tx_status __attribute__((section(".diag_safe")));
volatile uint32_t tmr0_irq_count __attribute__((section(".diag_safe")));
/* --- Step-1/hop-hypothesis breadcrumbs (Codex): survive the fault (.diag_safe,
 * NOLOAD) so a watchdog-OFF spin + SRAM snapshot reveals WHERE the fault occurred,
 * not just that it did. entered_connected/connected_tick/hop_advance == 0 at fault
 * exonerates the hand-rolled RTC hop; last_rf_op localizes the RF path in flight;
 * bb_cb_in_cs_count > 0 proves the VTF BB callback fired during a CSR-0x800
 * critical section (i.e. CSR 0x800 does NOT mask the fast-vectored BB -- Step 0). */
volatile uint32_t entered_connected_count __attribute__((section(".diag_safe")));
volatile uint32_t connected_tick_calls    __attribute__((section(".diag_safe")));
volatile uint32_t hop_advance_count       __attribute__((section(".diag_safe")));
volatile uint32_t rf_config_count         __attribute__((section(".diag_safe")));
volatile uint8_t  last_rf_op              __attribute__((section(".diag_safe"))); /* 1=start_rx 2=pair_bcast 3=respond_tx */
volatile uint8_t  last_cb_state          __attribute__((section(".diag_safe"))); /* rf_state at last BB callback */
volatile uint8_t  last_cb_sta            __attribute__((section(".diag_safe"))); /* sta arg at last BB callback */
volatile uint8_t  cb_in_cs               __attribute__((section(".diag_safe"))); /* live: inside a CSR-0x800 CS */
volatile uint32_t bb_cb_in_cs_count       __attribute__((section(".diag_safe"))); /* BB callback fired while cb_in_cs */
/* --- Phase-1 link-lock drop characterization (.diag_safe). Answers: at a drop is
 * the keyboard off-channel / lost lock (ll_dropN_*), does its per-channel catch
 * match the dongle's (ll_kbd_rx_ch), did the runaway/desync or step-clamp fire, and
 * is the servo one-sided (R8: plus/minus/noop). Read post-soak via OpenOCD. */
volatile uint32_t ll_kbd_rx_ch[5] __attribute__((section(".diag_safe")));   /* connected RX caught per data-channel idx */
volatile uint16_t ll_since_rx_max __attribute__((section(".diag_safe")));   /* max since_rx reached */
volatile uint32_t ll_desync_reseed __attribute__((section(".diag_safe")));  /* >64*interval runaway re-seed fired */
volatile uint32_t ll_step_clamp    __attribute__((section(".diag_safe")));  /* step>MAX clamp fired */
volatile uint32_t ll_servo_plus    __attribute__((section(".diag_safe")));  /* servo +1 */
volatile uint32_t ll_servo_minus   __attribute__((section(".diag_safe")));  /* servo -1 */
volatile uint32_t ll_servo_noop    __attribute__((section(".diag_safe")));  /* servo in-sync/deadzone no-op */
volatile uint32_t ll_drop_count    __attribute__((section(".diag_safe")));  /* supervision fired while CONNECTED */
volatile uint32_t ll_drop1_rtc     __attribute__((section(".diag_safe")));  /* RTC32K at first drop */
volatile uint32_t ll_dropN_rtc     __attribute__((section(".diag_safe")));  /* RTC32K at last drop */
volatile uint16_t ll_drop1_since_rx __attribute__((section(".diag_safe")));
volatile uint8_t  ll_drop1_idx      __attribute__((section(".diag_safe")));
volatile uint8_t  ll_drop1_channel  __attribute__((section(".diag_safe")));
volatile uint8_t  ll_drop1_provis   __attribute__((section(".diag_safe")));
volatile uint16_t ll_dropN_since_rx __attribute__((section(".diag_safe")));
volatile uint8_t  ll_dropN_idx      __attribute__((section(".diag_safe")));
volatile uint8_t  ll_dropN_channel  __attribute__((section(".diag_safe")));
volatile uint8_t  ll_dropN_provis   __attribute__((section(".diag_safe")));
/* --- Stock-interop connected-mode trace (.diag_safe): the first LL_TRACE_N RF
 * callbacks after enter_connected. Shows whether the stock dongle keeps polling
 * after our first catch (H1: dongle tears down on an unaccepted response) vs the
 * keyboard mis-phasing and missing every subsequent poll (H2). Per entry:
 * meta = sta | len<<8 | min(since_rx,255)<<16 | flags<<24;
 * flags = idx | connected<<3 | resp_pending<<4 | provisional<<5 | rsr_nz<<6. */
#define LL_TRACE_N 32
volatile uint32_t ll_trace_rtc[LL_TRACE_N]  __attribute__((section(".diag_safe")));
volatile uint32_t ll_trace_meta[LL_TRACE_N] __attribute__((section(".diag_safe")));
volatile uint16_t ll_trace_count __attribute__((section(".diag_safe")));
volatile uint8_t  ll_trace_armed __attribute__((section(".diag_safe")));
volatile uint32_t ll_resp_suppressed __attribute__((section(".diag_safe")));  /* responses skipped by STOCK_SUPPRESS_RESPONSES */
volatile uint32_t ll_boot_count __attribute__((section(".diag_safe")));  /* RF_TaskInit runs = reboots (NOT zeroed by the init block) */
/* --- Connected RX re-arm gap timing (.diag_safe). SysTick runs at Tsys
 * (60 MHz on this build), so these fields measure the RF_Shut/retune/RF_Rx
 * blind spot that RTC32K cannot resolve. */
volatile uint32_t ll_rx_rearm_count __attribute__((section(".diag_safe")));
volatile uint32_t ll_rx_rearm_gap_last __attribute__((section(".diag_safe")));
volatile uint32_t ll_rx_rearm_gap_min __attribute__((section(".diag_safe")));
volatile uint32_t ll_rx_rearm_gap_max __attribute__((section(".diag_safe")));
volatile uint32_t ll_rx_rearm_gap_sum __attribute__((section(".diag_safe")));
volatile uint32_t ll_rx_rearm_begin_sys __attribute__((section(".diag_safe")));
volatile uint32_t ll_rx_rearm_end_sys __attribute__((section(".diag_safe")));
volatile uint32_t ll_rx_cb_last_sys __attribute__((section(".diag_safe")));
volatile uint32_t ll_rx_since_rearm_sys __attribute__((section(".diag_safe")));
volatile uint8_t  ll_rx_rearm_last_idx __attribute__((section(".diag_safe")));
volatile uint8_t  ll_rx_rearm_last_channel __attribute__((section(".diag_safe")));
volatile uint8_t  ll_rx_rearm_last_provis __attribute__((section(".diag_safe")));
#define RF_DIAG_INC(x)    ((x)++)
#define RF_DIAG_SET(x, v) ((x) = (v))
#else
#define RF_DIAG_INC(x)    ((void)0)
#define RF_DIAG_SET(x, v) ((void)(v))
#endif

static uint16_t RF_ProcessEvent(uint8_t task_id, uint16_t events);
static uint8_t rf_configure(uint32_t access_addr);
static void rf_start_rx(uint8_t channel);
static void rf_pair_broadcast(void);
static void rf_enter_connected(void);
static void rf_do_response_tx(void);
static void rf_tmr0_stop(void);
static void rf_tmr0_arm(uint32_t count);

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint8_t mac_equal(const uint8_t *a, const uint8_t *b)
{
    for (uint8_t i = 0; i < 6; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static uint32_t rtc_read_32k(void)
{
    return *(volatile uint32_t *)R32_RTC_CNT_32K_ADDR;
}

#if RF_DIAG_COUNTERS
static uint32_t systick_read(void)
{
    return *(volatile uint32_t *)SYSTICK_CNT_ADDR;
}
#endif

/* The CH59x RTC32K reader can return a sample a few ticks BEHIND the previous
 * one (a known non-monotonic quirk -- the dongle uses the same 1024-tick
 * tolerance). Without this, one bad backward sample while hop_anchor is normally
 * only a few ticks behind `now` makes rtc_delta_32k() return ~RTC32K_WRAP, which
 * clamps step=5 and sends the hop into a same-channel runaway (the measured
 * cliff). Treat a small backstep as ZERO elapsed; only a near-modulus span is a
 * true 24h wrap. */
#define RTC32K_BACKSTEP_TOLERANCE  1024u

static uint32_t rtc_delta_32k(uint32_t now, uint32_t then)
{
    if (now >= then) {
        return now - then;
    }
    if ((then - now) <= RTC32K_BACKSTEP_TOLERANCE) {
        return 0u;   /* RTC32K backstep, not a wrap */
    }
    return RTC32K_WRAP - then + now;
}

static uint32_t rtc_backdate_32k(uint32_t t, uint32_t ticks)
{
    return (t >= ticks) ? (t - ticks) : (RTC32K_WRAP - (ticks - t));
}

static uint32_t rtc_add_32k(uint32_t t, uint32_t ticks)
{
    uint32_t s = t + ticks;
    while (s >= RTC32K_WRAP) {
        s -= RTC32K_WRAP;
    }
    return s;
}

/* SIGNED shortest-path phase delta (ref - a) around the RTC32K modulus, result in
 * (-WRAP/2, +WRAP/2]. Unlike rtc_delta_32k() this does NOT apply the 1024-tick
 * backstep tolerance -- the PLL servo needs the SIGN of a small negative error to
 * apply -1. (rtc_delta_32k collapses "anchor a few ticks ahead of ref" to 0, which
 * made the servo +1-only and let phase bias accumulate until the hop slipped
 * off-channel -- the connected-mode drop root cause, R8.) Servo-only: a spurious
 * RTC32K backstep costs at most one bounded -1 tick here (self-corrected next poll)
 * and cannot reach the elapsed-scheduling path, so the wrap/clamp cliff that the
 * 1024 tolerance protects rtc_delta_32k() from is not reintroduced. */
static int32_t rtc_signed_delta_32k(uint32_t ref, uint32_t a)
{
    uint32_t fwd = (ref >= a) ? (ref - a) : (RTC32K_WRAP - a + ref);  /* a -> ref, [0,WRAP) */
    if (fwd <= (RTC32K_WRAP >> 1)) {
        return (int32_t)fwd;                       /* ref at/ahead of anchor (>=0) */
    }
    return -(int32_t)(RTC32K_WRAP - fwd);          /* ref behind anchor (<0) */
}

/* QingKe global-interrupt save/restore (CSR 0x800 INTSYSCR, bits MPIE|MIE=0x88),
 * matching the CH592 EVT __risc_v_disable_irq/enable_irq. Used to make the
 * connected-hop anchor update atomic against the RX-ISR servo -- stock runs its
 * hop scheduler (0x20000E06) under the same IRQ save/restore. */
__attribute__((always_inline)) static inline uint32_t rf_irq_save(void)
{
    uint32_t r;
    __asm volatile ("csrrc %0, 0x800, %1" : "=r"(r) : "r"(0x88) : "memory");
#if RF_DIAG_COUNTERS
    cb_in_cs = 1;   /* IRQs now masked; BB callback flags a hit if it fires anyway */
#endif
    return r & 0x88;
}
__attribute__((always_inline)) static inline void rf_irq_restore(uint32_t s)
{
#if RF_DIAG_COUNTERS
    cb_in_cs = 0;   /* clear BEFORE re-enabling: no enabled-but-flagged window */
#endif
    uint32_t t;
    __asm volatile ("csrrs %0, 0x800, %1" : "=r"(t) : "r"(s) : "memory");
}

/* Wrap the app's TMOS timer-list mutators in a global-IRQ critical section as a
 * defensive measure. The v1.4.2 BLE lib guards its own TMOS list ops by masking
 * only BLEL (IRQ 21); our BB status callback is a fast-vectored (VTF) BLEB (IRQ 20)
 * interrupt v1.4.2 does not mask, so in principle a BB interrupt landing mid
 * app-list-mutation could race the main-loop walk (tmos_proces_system_time).
 * Disabling interrupts globally around each app list mutation closes that window.
 * (NB: the v1.4.2 "TMOS HardFault" this was aimed at proved to be a
 * debugger-perturbation artifact rather than an operational fault -- see
 * docs/TMOS_REVIEW.md -- and `bb_cb_in_cs_count` measured 0 BB-in-CS hits in a soak; the
 * wrap is cheap defense-in-depth and is retained. v1.00's spinlock did not need
 * it; harmless there.) */
static void rf_start_task_atomic(uint16_t evt, uint32_t ticks)
{
    uint32_t irq = rf_irq_save();
    tmos_start_task(rf_taskID, evt, ticks);
    rf_irq_restore(irq);
}
static void rf_stop_task_atomic(uint16_t evt)
{
    uint32_t irq = rf_irq_save();
    tmos_stop_task(rf_taskID, evt);
    rf_irq_restore(irq);
}
/* tmos_set_event() is also a non-atomic event-halfword RMW in v1.4.2, so post
 * RF task events under the same global mask from both ISR and main-loop paths. */
static void rf_set_event_atomic(uint16_t evt)
{
    uint32_t irq = rf_irq_save();
    tmos_set_event(rf_taskID, evt);
    rf_irq_restore(irq);
}

#if KBD_RF_CRYPT
/* A per-boot starting point for the CCM transmit counter.
 *
 * Not a security random: the counter travels in the clear, and CCM needs the
 * nonce to be UNIQUE, not secret. What it must do is differ from boot to boot,
 * because the receiver's session_id does not reliably differ -- its generator is
 * close to deterministic for a fixed device, and this bench saw the same value
 * return across reboots. Two deterministic halves make a repeated session_id
 * into certain keystream reuse; varying this one breaks that.
 *
 * RTC is the part that actually moves between boots; the chip UID only
 * separates units. Folded with a 32-bit mix so neighbouring RTC values do not
 * produce neighbouring starts, then bounded well below the top of the range so
 * exhaustion stays remote. */
static uint32_t rf_crypt_ctr_start(void)
{
    uint8_t uid[8] __attribute__((aligned(4))) = {0};
    uint32_t x;

    GET_UNIQUE_ID(uid);
    x = ((uint32_t)uid[0] | ((uint32_t)uid[1] << 8) |
         ((uint32_t)uid[2] << 16) | ((uint32_t)uid[3] << 24));
    x ^= ((uint32_t)uid[4] | ((uint32_t)uid[5] << 8) |
          ((uint32_t)uid[6] << 16) | ((uint32_t)uid[7] << 24));
    x ^= rtc_read_32k();
    x ^= systick_read();
    /* xorshift32 avalanche: spread neighbouring inputs across the range. */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x & KBD_CRYPT_CTR_START_MAX;
}
#endif

static uint32_t rf_bond_checksum(const rf_bond_record_t *rec)
{
    const uint8_t *p = (const uint8_t *)rec;
    uint32_t sum = 0;
    uint8_t len = (uint8_t)(sizeof(*rec) - sizeof(rec->checksum));

    for (uint8_t i = 0; i < len; i++) {
        sum += p[i];
    }
    return sum;
}

static uint8_t rf_bond_record_valid(const rf_bond_record_t *rec)
{
    if (rec->magic != RF_BOND_MAGIC || rec->version != RF_BOND_VERSION) {
        return 0;
    }
    if (rec->checksum != rf_bond_checksum(rec)) {
        return 0;
    }
    if (rec->session_aa == 0u || rec->session_aa == RF_DEFAULT_ACCESS_ADDR) {
        return 0;
    }
    return 1;
}

static void rf_build_bond_record(rf_bond_record_t *rec)
{
    tmos_memset(rec, 0, sizeof(*rec));
    rec->magic = RF_BOND_MAGIC;
    rec->version = RF_BOND_VERSION;
    rec->type_tag = stored_type_tag;
    rec->session_aa = stored_session_aa;
    for (uint8_t i = 0; i < 6; i++) {
        rec->dongle_mac[i] = stored_dongle_mac[i];
    }
#if KBD_RF_CRYPT
    rec->flags = stored_bond_flags;
    /* Canonical form: no key provisioned => the field is zero, so a stale key
     * can never sit behind a cleared flag. */
    if (stored_bond_flags & RF_BOND_FLAG_ENC_KEY) {
        for (uint8_t i = 0; i < KBD_CRYPT_KEY_BYTES; i++) {
            rec->link_key[i] = stored_link_key[i];
        }
    }
#endif
    rec->checksum = rf_bond_checksum(rec);
}

static uint8_t rf_bond_record_matches(const rf_bond_record_t *a,
                                      const rf_bond_record_t *b)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    for (uint8_t i = 0; i < sizeof(*a); i++) {
        if (pa[i] != pb[i]) {
            return 0;
        }
    }
    return 1;
}

static uint8_t rf_bond_flash_is_erased(const rf_bond_record_t *rec)
{
    const uint8_t *p = (const uint8_t *)rec;

    for (uint8_t i = 0; i < sizeof(*rec); i++) {
        if (p[i] != 0xffu) {
            return 0;
        }
    }
    return 1;
}

static uint8_t rf_load_bond_from_flash(void)
{
    rf_bond_record_t rec __attribute__((aligned(4)));

    if (EEPROM_READ(RF_BOND_EEPROM_OFF, &rec, sizeof(rec)) != 0) {
        return 0;
    }
    if (!rf_bond_record_valid(&rec)) {
        return 0;
    }

    stored_session_aa = rec.session_aa;
    stored_type_tag = rec.type_tag;
    for (uint8_t i = 0; i < 6; i++) {
        stored_dongle_mac[i] = rec.dongle_mac[i];
    }
#if KBD_RF_CRYPT
    stored_bond_flags = rec.flags;
    for (uint8_t i = 0; i < KBD_CRYPT_KEY_BYTES; i++) {
        stored_link_key[i] = rec.link_key[i];
    }
    /* Install the key once, here at boot, where the schedule is off the poll
     * grid. The per-session nonce arrives later, from the receiver's announce. */
    if (rf_bond_enc_active()) {
        kbd_crypt_install_key(stored_link_key, rf_crypt_ctr_start());
    } else {
        kbd_crypt_clear();
    }
#endif
    has_bond = 1;
    return 1;
}

static void rf_save_bond_to_flash(void)
{
    rf_bond_record_t want __attribute__((aligned(4)));
    rf_bond_record_t cur __attribute__((aligned(4)));
    uint8_t have_cur = 0;

    if (!has_bond) {
        return;
    }

    rf_build_bond_record(&want);
    if (EEPROM_READ(RF_BOND_EEPROM_OFF, &cur, sizeof(cur)) == 0) {
        have_cur = 1;
        if (rf_bond_record_valid(&cur) && rf_bond_record_matches(&cur, &want)) {
            return;
        }
    }

    if (!have_cur || !rf_bond_flash_is_erased(&cur)) {
        if (EEPROM_ERASE(RF_BOND_EEPROM_OFF, RF_BOND_EEPROM_ERASE_LEN) != 0) {
            return;
        }
    }
    (void)EEPROM_WRITE(RF_BOND_EEPROM_OFF, &want, sizeof(want));
}

static void rf_clear_bond_ram(void)
{
    has_bond = 0;
    stored_session_aa = 0;
    stored_type_tag = 0;
    tmos_memset(stored_dongle_mac, 0, sizeof(stored_dongle_mac));
#if KBD_RF_CRYPT
    /* Unpairing must take the key with it -- a cleared bond that kept its key
     * would re-arm encryption against a peer we no longer have a session with. */
    stored_bond_flags = 0;
    tmos_memset(stored_link_key, 0, sizeof(stored_link_key));
    kbd_crypt_clear();
#endif
}

static void rf_clear_bond_flash(void)
{
    (void)EEPROM_ERASE(RF_BOND_EEPROM_OFF, RF_BOND_EEPROM_ERASE_LEN);
}

/* PLL servo (stock 0x20000D60): pull the hop anchor toward the reference time
 * (the poll's start, now-(12+(len>>3))) by EXACTLY +/-1 RTC32K tick, shortest
 * direction around the wrap. Pure +/-1 like stock -- no snap. (A snap to the ref
 * over-corrects on RX-callback jitter and can slip the data-channel index.)
 *
 * Uses the SIGNED comparator rtc_signed_delta_32k(): the previous version routed
 * through rtc_delta_32k() whose 1024-tick backstep tolerance collapsed the small
 * "anchor ahead of ref" case to 0, making the servo +1-ONLY (-1 was unreachable).
 * That one-sided servo let phase bias accumulate until the timed hop slipped off
 * the dongle's poll channel -> lock loss -> the connected-mode drops (R8, bench
 * proven RF-independent). A tiny symmetric deadband suppresses +/-1 chatter on
 * sub-tick jitter; real errors get corrected in BOTH directions. */
/* LINK_LOCK_SERVO_FIX gates the R8 fix so the ABAB bench crossover can build the old
 * (+1-only) and new (bipolar) servo from one tree (`make LINK_LOCK_SERVO_FIX=0` = old). */
#ifndef LINK_LOCK_SERVO_FIX
#define LINK_LOCK_SERVO_FIX 1
#endif
#define SERVO_DEADBAND  1   /* |phase err| <= this many RTC32K ticks -> no correction */
__HIGH_CODE
static void hop_servo(uint32_t ref)
{
#if LINK_LOCK_SERVO_FIX
    int32_t err = rtc_signed_delta_32k(ref, hop_anchor);   /* ref - anchor, signed */

    if (err > SERVO_DEADBAND) {
        RF_DIAG_INC(ll_servo_plus);
        hop_anchor = rtc_add_32k(hop_anchor, 1);            /* ref ahead: +1 */
    } else if (err < -SERVO_DEADBAND) {
        RF_DIAG_INC(ll_servo_minus);
        hop_anchor = rtc_backdate_32k(hop_anchor, 1);       /* ref behind: -1 (mod wrap) */
    } else {
        RF_DIAG_INC(ll_servo_noop);                         /* within deadband: in sync */
    }
#else
    /* Original R8 servo (+1-only in practice: rtc_delta_32k swallows small -err). */
    uint32_t a = hop_anchor;
    uint32_t fwd = rtc_delta_32k(ref, a);
    uint32_t bwd = RTC32K_WRAP - fwd;
    if (fwd == 0) { RF_DIAG_INC(ll_servo_noop); return; }
    if (fwd <= bwd) { RF_DIAG_INC(ll_servo_plus);  hop_anchor = rtc_add_32k(a, 1); }
    else            { RF_DIAG_INC(ll_servo_minus); hop_anchor = rtc_backdate_32k(a, 1); }
#endif
}

#if RF_DIAG_COUNTERS
/* Record one RF callback into the connected-mode trace ring (armed at
 * rf_enter_connected, self-stops after LL_TRACE_N entries). */
__HIGH_CODE
static void ll_trace_rec(uint8_t sta, uint8_t rsr, const uint8_t *rxBuf)
{
    uint16_t n, sr;
    uint8_t len, flags, trace_since;
    if (!ll_trace_armed) return;
    n = ll_trace_count;
    if (n >= LL_TRACE_N) { ll_trace_armed = 0; return; }
    len = (uint8_t)((sta == RX_MODE_RX_DATA && rxBuf != 0) ? rxBuf[1] : 0u);
    flags = (uint8_t)(prev_data_idx & 0x07u)
          | (uint8_t)((rf_state == RF_STATE_CONNECTED) ? 0x08u : 0u)
          | (uint8_t)((response_pending != 0) ? 0x10u : 0u)
          | (uint8_t)((provisional != 0) ? 0x20u : 0u)
          | (uint8_t)((sta == RX_MODE_RX_DATA && rsr != 0) ? 0x40u : 0u);
    ll_trace_rtc[n] = rtc_read_32k();
    sr = since_rx;
    trace_since = (uint8_t)((sr > 0xffu) ? 0xffu : sr);
    ll_trace_meta[n] = (uint32_t)sta | ((uint32_t)len << 8)
                     | ((uint32_t)trace_since << 16) | ((uint32_t)flags << 24);
    ll_trace_count = (uint16_t)(n + 1u);
}

__HIGH_CODE
static void ll_record_rx_rearm_gap(uint32_t begin_sys, uint32_t end_sys)
{
    uint32_t gap = end_sys - begin_sys;

    ll_rx_rearm_count++;
    ll_rx_rearm_gap_last = gap;
    if (ll_rx_rearm_gap_min == 0u || gap < ll_rx_rearm_gap_min) {
        ll_rx_rearm_gap_min = gap;
    }
    if (gap > ll_rx_rearm_gap_max) {
        ll_rx_rearm_gap_max = gap;
    }
    ll_rx_rearm_gap_sum += gap;
    ll_rx_rearm_begin_sys = begin_sys;
    ll_rx_rearm_end_sys = end_sys;
    ll_rx_rearm_last_idx = prev_data_idx;
    ll_rx_rearm_last_channel = rf_channel;
    ll_rx_rearm_last_provis = provisional;
}
#endif

__HIGH_CODE
void RF_2G4StatusCallBack(uint8_t sta, uint8_t rsr, uint8_t *rxBuf)
{
    RF_DIAG_INC(rf_cb_count[0]);
#if KBD_CRYPT_BENCH_KEY
    /* Bench: this callback preempts the task context that runs seal_begin /
     * the self-verify; the vendor IRQ path reads/clears AES_STA. Count the
     * overlaps, total and per-seal. */
    if (kbd_crypt_in_aes) {
        kbd_crypt_bb_during_aes++;
        if (kbd_crypt_seal_bb != 0xFFu) {
            kbd_crypt_seal_bb++;
        }
    }
#endif
#if RF_DIAG_COUNTERS
    ll_trace_rec(sta, rsr, rxBuf);
#endif
#if RF_DIAG_COUNTERS
    last_cb_sta = sta;
    last_cb_state = rf_state;
    if (cb_in_cs) {
        bb_cb_in_cs_count++;   /* VTF BB fired during a CSR-0x800 CS -> mask leak */
    }
#endif

    if (sta == RX_MODE_RX_DATA) {
        RF_DIAG_INC(rf_cb_count[(rsr == 0) ? 1 : 2]);
        if (rsr != 0 || rxBuf == 0) {
            rf_set_event_atomic(RF_EVT_RX_RESTART);
            return;
        }

        RF_DIAG_INC(rf_valid_rx_count);
        rf_rssi = (int8_t)rxBuf[0];

        if (rf_state == RF_STATE_PAIRING && rxBuf[1] == 15 && rxBuf[6] < 5) {
            const uint8_t *payload = &rxBuf[2];
            uint32_t session_aa = read_be32(payload);
            uint16_t interval = (uint16_t)payload[5] |
                                ((uint16_t)payload[6] << 8);
            const uint8_t *dongle_mac = &payload[9];

            if (!has_bond) {
                for (uint8_t i = 0; i < 6; i++) {
                    stored_dongle_mac[i] = dongle_mac[i];
                }
                stored_session_aa = session_aa;
                stored_type_tag = payload[4];
                has_bond = 1;
                rf_access_addr = stored_session_aa;
                rf_set_event_atomic(RF_EVT_RX_RESTART);
                return;
            }

            if (mac_equal(stored_dongle_mac, dongle_mac)) {
                uint16_t eff_interval = interval ? interval : RF_PAIR_INTERVAL;
                /* Reject (don't clamp) a pathological advertised poll interval:
                 * clamping to a value != the dongle's real interval would
                 * silently desync the hop schedule. [20,40] brackets the
                 * validated 28. These units are RTC32K hop slots, not TMOS
                 * supervision ticks, so the bound is a protocol sanity check rather
                 * than a since_rx wrap guard. */
                if (eff_interval < 20u || eff_interval > 40u) {
                    rf_set_event_atomic(RF_EVT_RX_RESTART);
                    return;
                }
#if STOCK_FORCE_CONN_INTERVAL
                eff_interval = STOCK_FORCE_CONN_INTERVAL;
#endif
                pending_session_aa = session_aa;
                pending_type_tag = payload[4];
                pending_interval = eff_interval;
                /* Seed the hop anchor at this 2nd 15-byte's start (now-13),
                 * matching stock (PROTOCOL.md): the connected time-based hop and
                 * the PLL servo both reference this anchor. */
                pending_anchor = rtc_backdate_32k(rtc_read_32k(),
                                                  RF_HOP_ANCHOR_BACKDATE);
                rf_set_event_atomic(RF_EVT_ENTER_CONNECTED);
                return;
            }
        }

        if (rf_state == RF_STATE_CONNECTED) {
            uint8_t len = rxBuf[1];

            /* Genuine connected polls are LEN 1 (poll), 3 (LED relay) or 10 (HID).
             * Any other length (incl. the dongle's LEN-15 re-key/pair bursts) is
             * ignored here -- responding to them disrupts the lock. (Measured: the
             * keyboard never even receives the EV10 LEN-15 re-key before a drop --
             * the drop is loss of channel lock, not an EV10-rekey-ignore.) */
            if (len != 1u && len != 3u && len != 10u
#if KBD_RF_CRYPT
                /* The receiver announces a fresh session nonce in place of a
                 * poll, so this length has to be let through or encryption can
                 * never start. It is authenticated, but not here: copy it out
                 * and verify in task context (hal_aes must not run in an ISR). */
                && len != KBD_CRYPT_LEN_SESSION
#endif
                ) {
                rf_set_event_atomic(RF_EVT_RX_RESTART);
                return;
            }
#if RF_DIAG_COUNTERS
            {
                uint32_t cb_sys = systick_read();
                ll_rx_cb_last_sys = cb_sys;
                ll_rx_since_rearm_sys = cb_sys - ll_rx_rearm_end_sys;
            }
#endif
            uint8_t rx_ctrl = rxBuf[2];   /* read only after the length is validated */

            /* PLL servo (stock 0x20001192 -> 0x20000D60): nudge the hop anchor
             * toward this poll's start time INLINE in the RX ISR, exactly like
             * stock. RF_ConnectedTick's anchor update runs in a critical section
             * so this ISR write can't corrupt it. Stock backdates by 12+(len>>3):
             * LEN 1/3 polls use 12, LEN 10/15 use 13. */
            {
                uint32_t poll_ref = rtc_backdate_32k(rtc_read_32k(),
                                                     RF_POLL_BACKDATE_BASE + (len >> 3));
#if STOCK_ACQ_SNAP
                if (provisional) {
                    hop_anchor = poll_ref;   /* acquisition: coarse-snap the hop grid to this poll */
                } else {
                    hop_servo(poll_ref);     /* steady state: ±1 servo */
                }
#else
                hop_servo(poll_ref);
#endif
            }
#if RF_DIAG_COUNTERS
            if (prev_data_idx < NUM_DATA_CHANNELS) ll_kbd_rx_ch[prev_data_idx]++;
#endif
            since_rx = 0;
#if !STOCK_PARK_AFTER_CATCH
            provisional = 0;   /* locked: caught a poll, resume tracking */
#endif
            if (bond_save_pending) {
                bond_save_pending = 0;
                rf_set_event_atomic(RF_EVT_SAVE_BOND);
            }
            /* STOCK_PARK_AFTER_CATCH: stay parked on the catch channel (never
             * advance the hop) -- H1/H2 probe: if the dongle keeps polling it
             * sweeps back here every 5 intervals and we keep catching. */

            if ((rx_ctrl ^ tx_ctrl) & 0x01) {
                tx_ctrl = (uint8_t)(((tx_ctrl & (uint8_t)~0x01) |
                                     (rx_ctrl & 0x01)) ^ 0x02);
            }

            if (len == 3 && rxBuf[3] == 0xA1 && rxBuf[4] != last_led_sent) {
                pending_led = rxBuf[4];
                rf_set_event_atomic(RF_EVT_NOTIFY_LED);
            }
#if KBD_RF_CRYPT
            /* Session announce: copy it out and defer. Verifying needs the AES
             * engine, which must not run here -- same discipline the receiver
             * uses for its own decrypt path. Nothing is adopted until the tag
             * checks out in task context. */
            if (len == KBD_CRYPT_LEN_SESSION && rxBuf[3] == KBD_CRYPT_TAG_SESSION
                && !crypt_session_rx_pending) {
                for (uint8_t i = 0; i < KBD_CRYPT_LEN_SESSION; i++) {
                    crypt_session_rx[i] = rxBuf[2 + i];
                }
                crypt_session_rx_pending = 1;
                kbd_crypt_sess_rx++;
                rf_set_event_atomic(RF_EVT_CRYPT_SESSION);
            }
            /* Count every connected reception against the receiver's silence
             * guard; only a frame we actually authenticated resets it. */
            if (crypt_polls_since_auth < 0xFFu) {
                crypt_polls_since_auth++;
            }
            if (crypt_polls_since_auth >= KBD_CRYPT_KEEPALIVE_POLLS) {
                crypt_keepalive_due = 1;
            }
#endif

            /* Re-arm the supervision timer in the main loop (RF_ConnectedTick),
             * NOT here: tmos_start/stop_task are not safe from this ISR (see the
             * supervision_kick note above). */
            supervision_kick = 1;

            /* Latch the response channel + ctrl NOW: the response must go
             * out on the channel THIS poll arrived on, and RF_ConnectedTick must
             * not retune/shut the radio while it is pending. Then arm the TMR0
             * one-shot turnaround; its IRQ posts RF_EVT_RESPOND and the main-loop
             * handler TXes on the latched channel, landing the response in the
             * dongle's post-poll RX window. */
            response_channel = rf_channel;
            response_ctrl = tx_ctrl;
            response_armed_rtc = rtc_read_32k();
            response_pending = 1;
            rf_tmr0_arm(RF_TURNAROUND_COUNT);
            return;
        }

        rf_set_event_atomic(RF_EVT_RX_RESTART);
        return;
    }

    if (sta == TX_MODE_TX_FINISH) {
        RF_DIAG_INC(rf_cb_count[3]);
        response_pending = 0;   /* response done -> hop may retune again */
        rf_set_event_atomic(RF_EVT_RX_RESTART);
        return;
    }

    if (sta == TX_MODE_TX_FAIL) {
        RF_DIAG_INC(rf_cb_count[4]);
        response_pending = 0;
        rf_set_event_atomic(RF_EVT_RX_RESTART);
        return;
    }

    RF_DIAG_INC(rf_cb_count[5]);
    rf_set_event_atomic(RF_EVT_RX_RESTART);
}

/* Build + transmit the poll response on the latched channel: LEN=10 HID while a
 * changed report is pending (a few resends), else a LEN=1 [ctrl] keepalive. TX
 * skips the full RF_Config (AA/mode still set from the last rf_start_rx) to cut
 * turnaround latency; TX_FINISH clears response_pending and re-arms RX. Called
 * from the main-loop RF_EVT_RESPOND handler, OR (STOCK_ISR_FAST_RESPONSE) directly
 * from the TMR0 turnaround ISR. __HIGH_CODE + register-level RF ops keep it ISR-safe. */
__HIGH_CODE
static void rf_do_response_tx(void)
{
    uint8_t tx_len;

    if (!response_pending) {
        return;
    }

#if STOCK_SUPPRESS_RESPONSES
    if (ll_resp_suppressed < (uint32_t)STOCK_SUPPRESS_RESPONSES) {
        ll_resp_suppressed++;
        response_pending = 0;
        rf_set_event_atomic(RF_EVT_RX_RESTART);
        return;
    }
#endif
    tx_payload[0] = response_ctrl;
    tx_len = 1;
#if KBD_RF_CRYPT
    /* Gate on whether the BOND requires encryption, NOT on whether a session is
     * currently usable. Those differ, and the difference leaked keystrokes:
     * kbd_crypt_active() additionally demands an adopted session, and
     * rf_enter_connected() ends the session on EVERY connect, so between connect
     * and the receiver's session announce this test was false on an
     * encryption-required bond and execution fell through to the plaintext
     * branch below. A report pending across that window went out as
     * [ctrl][A1][report] in clear. The receiver discards it as a downgrade, so
     * the link still worked and the leak was invisible -- but a passive listener
     * gets the keystroke, which is the exact thing this feature exists to stop.
     *
     * With the bond-level predicate, an encryption-required bond that has no
     * usable session simply finds no sealed frame and sends the bare ack. */
    if (rf_bond_enc_active()) {
        /* Either the pre-sealed frame goes out or the bare ack does -- never
         * the plaintext report. */
        if (hid_resend || crypt_keepalive_due) {
            uint8_t sealed_len = 0;
            /* No cipher work happens here: seal_finish only selects the tag
             * that seal_begin already computed for this ctrl. */
            if (kbd_crypt_seal_finish(response_ctrl, tx_payload, &sealed_len)
                    == KBD_CRYPT_OK) {
                tx_len = sealed_len;
                if (hid_resend) {
                    hid_resend--;
                }
                crypt_keepalive_due = 0;
                crypt_polls_since_auth = 0;
                tx_payload[0] = response_ctrl;
                kbd_crypt_tx_sealed++;
#if KBD_CRYPT_BENCH_KEY
                /* Bench: snapshot the exact bytes handed to RF_Tx below; the
                 * next CRYPT_ARM re-verifies them in task context. */
                kbd_crypt_bench_snapshot(tx_payload, tx_len);
#endif
            } else {
                kbd_crypt_seal_miss++;
            }
            /* Seal the next one from task context, whether or not this slot
             * found a frame ready. */
            rf_set_event_atomic(RF_EVT_CRYPT_ARM);
        }
    } else
#endif
    if (hid_resend) {
        tx_payload[1] = 0xA1;
        for (uint8_t i = 0; i < 8; i++) {
            tx_payload[2 + i] = hid_report[i];
        }
        tx_len = 10;
        hid_resend--;
    }
    RF_DIAG_SET(last_rf_op, 3);
    RF_Shut();
#if !STOCK_RESP_NO_SETCH
    /* Redundant: the radio is already on the poll channel from rf_start_rx, and
     * re-setting it forces a ~150us PLL relock -- pushing the response outside the
     * stock dongle's post-poll RX window. The stock keyboard (firmwareB.bin
     * 0x20000f06) TXes the response with NO RF_SetChannel for exactly this reason. */
    RF_SetChannel(response_channel);
#endif
    uint8_t tx_status = RF_Tx(tx_payload, tx_len, 0xFF, 0xFF);
    RF_DIAG_SET(rf_last_tx_status, tx_status);
    RF_DIAG_INC(rf_connected_tx_count);
    if (tx_status != 0) {
        /* TX did not start -> no TX_FINISH will come; don't block the hop. */
        response_pending = 0;
        rf_set_event_atomic(RF_EVT_RX_RESTART);
    }
}

static uint16_t RF_ProcessEvent(uint8_t task_id, uint16_t events)
{
    if (events & SYS_EVENT_MSG) {
        uint8_t *msg = tmos_msg_receive(task_id);
        if (msg != NULL) {
            tmos_msg_deallocate(msg);
        }
        return events ^ SYS_EVENT_MSG;
    }

    if (events & RF_EVT_START) {
        rf_state = RF_STATE_IDLE;
        rf_access_addr = RF_DEFAULT_ACCESS_ADDR;
        rf_channel = pair_channels[0];
        RF_Shut();
        return events ^ RF_EVT_START;
    }

    if (events & RF_EVT_RX_RESTART) {
        if (rf_state == RF_STATE_PAIRING || rf_state == RF_STATE_CONNECTED) {
            rf_start_rx(rf_channel);   /* re-arm on the current channel */
        }
        return events ^ RF_EVT_RX_RESTART;
    }

    if (events & RF_EVT_PAIR_BCAST) {
        if (rf_state == RF_STATE_PAIRING) {
            rf_pair_broadcast();
        }
        return events ^ RF_EVT_PAIR_BCAST;
    }

    if (events & RF_EVT_PAIR_TIMEOUT) {
        if (rf_state == RF_STATE_PAIRING) {
            rf_state = RF_STATE_IDLE;
            RF_Shut();
        }
        return events ^ RF_EVT_PAIR_TIMEOUT;
    }

    if (events & RF_EVT_ENTER_CONNECTED) {
        rf_enter_connected();
        return events ^ RF_EVT_ENTER_CONNECTED;
    }

    if (events & RF_EVT_RESPOND) {
        /* In STOCK_ISR_FAST_RESPONSE builds the TMR0 ISR TXes directly and never
         * posts this event; this main-loop path is the default (deferred) one. */
        if (!STOCK_ISR_FAST_RESPONSE && rf_state == RF_STATE_CONNECTED) {
            rf_do_response_tx();
        }
        return events ^ RF_EVT_RESPOND;
    }

    if (events & RF_EVT_NOTIFY_LED) {
        KeyboardUart_SendLed(pending_led);
        last_led_sent = pending_led;
        return events ^ RF_EVT_NOTIFY_LED;
    }

#if KBD_RF_CRYPT
    if (events & RF_EVT_CRYPT_SESSION) {
        uint8_t frame[KBD_CRYPT_LEN_SESSION];
        uint32_t sid = 0;

        for (uint8_t i = 0; i < KBD_CRYPT_LEN_SESSION; i++) {
            frame[i] = crypt_session_rx[i];
        }
        crypt_session_rx_pending = 0;
        /* A frame that does not authenticate is simply dropped: it is noise or
         * a forgery, and adopting from it would hand an attacker our session. */
        if (kbd_crypt_verify_session(frame, KBD_CRYPT_LEN_SESSION, &sid)
                == KBD_CRYPT_OK) {
            kbd_crypt_adopt_session(sid);
            kbd_crypt_sess_ok++;
            rf_set_event_atomic(RF_EVT_CRYPT_ARM);
        } else {
            kbd_crypt_sess_bad++;
        }
        return events ^ RF_EVT_CRYPT_SESSION;
    }

    if (events & RF_EVT_CRYPT_ARM) {
        /* Task context: the whole CCM for the next uplink frame, so the
         * response path has only a buffer copy left to do. */
        if (kbd_crypt_active() && !kbd_crypt_seal_pending()) {
            rf_crypt_arm(tx_ctrl);
        }
        return events ^ RF_EVT_CRYPT_ARM;
    }
#endif

    if (events & RF_EVT_SAVE_BOND) {
        rf_save_bond_to_flash();
        return events ^ RF_EVT_SAVE_BOND;
    }

    if (events & RF_EVT_DISCONNECT) {
        if (rf_state == RF_STATE_CONNECTED) {
#if RF_DIAG_COUNTERS
            /* Drop snapshot (Codex): capture the keyboard's lock state at the moment
             * supervision fires. High since_rx => keyboard stopped hearing polls
             * (lost lock / dongle stopped). idx/channel = where it was when it lost
             * it. rtc lets us correlate against the dongle's last_rx_rtc. */
            ll_drop_count++;
            uint32_t drtc = rtc_read_32k();
            if (ll_drop_count == 1) {
                ll_drop1_since_rx = since_rx; ll_drop1_idx = prev_data_idx;
                ll_drop1_channel = rf_channel; ll_drop1_provis = provisional;
                ll_drop1_rtc = drtc;
            }
            ll_dropN_since_rx = since_rx; ll_dropN_idx = prev_data_idx;
            ll_dropN_channel = rf_channel; ll_dropN_provis = provisional;
            ll_dropN_rtc = drtc;
#endif
            rf_tmr0_stop();
            response_pending = 0;
            bond_save_pending = 0;
            rf_state = RF_STATE_IDLE;
            RF_Shut();
            /* Clean teardown to IDLE + 5B33; the host paces the reconnect with
             * A6 30. (Firmware-side immediate auto-re-pair livelocks: it re-pairs
             * faster than the dongle can settle into connected polling.) */
            KeyboardUart_SendStatus(0x33);
        }
        return events ^ RF_EVT_DISCONNECT;
    }

    return 0;
}

static uint8_t rf_configure(uint32_t access_addr)
{
    rfConfig_t cfg;

    tmos_memset(&cfg, 0, sizeof(cfg));
    cfg.LLEMode = RF_LLE_MODE;
    cfg.Channel = 0;
    cfg.accessAddress = access_addr;
    cfg.CRCInit = RF_CRC_INIT;
    cfg.rfStatusCB = RF_2G4StatusCallBack;
    cfg.RxMaxlen = RF_RX_MAX_LEN;
#if KBD_RF_CRYPT
    /* The memset above leaves TxMaxlen at 0 while the library documents a
     * default of 251. Ten-byte frames evidently pass regardless, but an
     * encrypted frame is 22, so stop relying on that and state the bound. */
    cfg.TxMaxlen = RF_RX_MAX_LEN;
#endif

    RF_DIAG_INC(rf_config_count);
    uint8_t cfg_status = RF_Config(&cfg);
    RF_DIAG_SET(rf_last_config_status, cfg_status);
    RF_SetChannel(rf_channel);
    return cfg_status;
}

/* [STEP1 experiment] RF_Config re-inits the RF/LLE stack and only needs to run
 * when the access address changes (pairing AA -> connected session AA). Calling
 * it on every ~17ms hop re-arm is the suspected v1.4.2 TMOS-timer-list corruption
 * trigger (RF-stack re-entry racing the LLE/TMOS scheduler). Config once per AA;
 * between hops just retune. (The connected response-TX path already skips
 * RF_Config for the same reason -- see the RF_EVT_RESPOND handler.) */
static uint32_t rf_configured_aa;
static void rf_configure_if_needed(uint32_t access_addr)
{
    if (rf_configured_aa != access_addr) {
        uint8_t cfg_status = rf_configure(access_addr);
        if (cfg_status == 0) {
            rf_configured_aa = access_addr;
        }
    } else {
        RF_SetChannel(rf_channel);   /* same AA: retune only, no RF_Config */
    }
}

__HIGH_CODE
static void rf_start_rx(uint8_t channel)
{
#if RF_DIAG_COUNTERS
    uint8_t diag_connected = (rf_state == RF_STATE_CONNECTED);
    uint32_t diag_begin_sys = diag_connected ? systick_read() : 0u;
#endif
    RF_DIAG_SET(last_rf_op, 1);
    RF_Shut();
    /* Commit rf_channel WHILE the radio is shut (R1): rf_channel is the channel
     * RX is physically armed on, and the RX ISR latches response_channel from it
     * (line ~311). Assigning it only here -- after RF_Shut, before the RF_Rx
     * re-arm -- guarantees a poll can never latch a look-ahead channel the radio
     * is not yet tuned to. The caller passes the target channel instead of
     * pre-setting rf_channel. */
    rf_channel = channel;
    rf_configure_if_needed(rf_access_addr);   /* RF_Config only on AA change; else retune */
    uint8_t rx_status = RF_Rx(NULL, 0, 0xFF, 0xFF);
    RF_DIAG_SET(rf_last_rx_status, rx_status);
#if RF_DIAG_COUNTERS
    if (diag_connected) {
        ll_record_rx_rearm_gap(diag_begin_sys, systick_read());
    }
#endif
}

#if KBD_RF_CRYPT
static void rf_pair_send_cap_advert(void);
#endif

static void rf_pair_broadcast(void)
{
    RF_DIAG_SET(last_rf_op, 2);
    uint8_t dwell = (uint8_t)(pair_bcast_count / RF_PAIR_DWELL_BCASTS);
    uint8_t pair_channel_idx = (uint8_t)(dwell % NUM_PAIR_CHANNELS);

    rf_channel = pair_channels[pair_channel_idx];

#if KBD_RF_CRYPT
    /* One slot in four advertises the encryption capability instead of the
     * beacon. It must arrive BEFORE the receiver commits the bond, because the
     * capability is read at commit time and the receiver can accept the very
     * first beacon it hears -- so lead with two, then repeat occasionally in
     * case both were lost. Advertising only on a late slot leaves the bond
     * recorded as not-capable, and encryption then stays off no matter how the
     * key is provisioned (bench-observed, 2026-08-10).
     *
     * These replace a beacon slot rather than doubling up: TX completion is
     * asynchronous, so two transmissions in one slot would need sequencing. The
     * receiver only answers beacons, so an advert slot costs 20 ms of pairing
     * latency and nothing else. */
    if (pair_bcast_count < 2u || (pair_bcast_count & 0x07u) == 0x07u) {
        pair_bcast_count++;
        rf_pair_send_cap_advert();
        return;
    }
#endif

    for (uint8_t i = 0; i < 6; i++) {
        pair_payload[i] = keyboard_mac[i];
    }
    pair_payload[6] = (uint8_t)(RF_PAIR_INTERVAL & 0xFF);
    pair_payload[7] = (uint8_t)(RF_PAIR_INTERVAL >> 8);
    pair_payload[8] = (uint8_t)(RF_PAIR_TIMEOUT_VALUE & 0xFF);
    pair_payload[9] = (uint8_t)(RF_PAIR_TIMEOUT_VALUE >> 8);

    RF_Shut();
    rf_configure_if_needed(rf_access_addr);
    uint8_t tx_status = RF_Tx(pair_payload, sizeof(pair_payload), 0xFF, 0xFF);
    RF_DIAG_SET(rf_last_tx_status, tx_status);

    RF_DIAG_INC(rf_pair_bcast_count);
    pair_bcast_count++;
    rf_start_task_atomic(RF_EVT_PAIR_BCAST, RF_PAIR_BCAST_TICKS);
}

#if KBD_RF_CRYPT
/* Tell the receiver we can do link encryption, so it records the capability on
 * the bond it is about to write. Purely additive: the beacon carries no tag
 * byte and the receiver classifies pair traffic by length, so this LEN-3 frame
 * is a separate transmission that a receiver without the feature ignores.
 *
 * It rides one broadcast slot in four rather than doubling every slot: TX
 * completion is asynchronous (TX_MODE_TX_FINISH re-arms RX), so two
 * back-to-back RF_Tx calls in one slot would need sequencing, and the receiver
 * only has to see this once during the pairing window. */
static void rf_pair_send_cap_advert(void)
{
    uint8_t cap[KBD_CRYPT_LEN_CAP];

    cap[0] = 0u;                         /* ctrl: ignored by the receiver here */
    cap[1] = KBD_CRYPT_TAG_CAP;
    cap[2] = KBD_CRYPT_CAP_VERSION;

    RF_Shut();
    rf_configure_if_needed(rf_access_addr);
    (void)RF_Tx(cap, sizeof(cap), 0xFF, 0xFF);
    rf_start_task_atomic(RF_EVT_PAIR_BCAST, RF_PAIR_BCAST_TICKS);
}
#endif

static void rf_enter_connected(void)
{
    RF_DIAG_INC(entered_connected_count);
    /* Only ever reached from PAIRING (the connected path drops LEN-15 re-keys), so
     * this is always a real IDLE/PAIRING->CONNECTED transition -- announce 5B32. */
    rf_stop_task_atomic(RF_EVT_PAIR_BCAST);
    rf_stop_task_atomic(RF_EVT_PAIR_TIMEOUT);

    stored_session_aa = pending_session_aa;
    stored_type_tag = pending_type_tag;
    rf_access_addr = stored_session_aa;
    rf_conn_interval = pending_interval;
    prev_data_idx = (uint8_t)((stored_type_tag + STOCK_CONNECT_IDX_BIAS) % NUM_DATA_CHANNELS);
    tx_ctrl = 0x02;
#if KBD_RF_CRYPT
    /* We always advertise the capability while pairing, so the bond is capable
     * from our side; encryption still needs a key before it goes active. */
    stored_bond_flags |= RF_BOND_FLAG_ENC_CAPABLE;
    /* Each connection gets a fresh session from the receiver's announce. Drop
     * any session carried over from the previous one so nothing is transmitted
     * under a session the receiver has already replaced. */
    kbd_crypt_end_session();
    crypt_session_rx_pending = 0;
    crypt_polls_since_auth = 0;
    crypt_keepalive_due = 0;
#endif
    hop_anchor = pending_anchor;   /* seeded at the 2nd 15-byte (rx-13) */
    since_rx = 0;
    response_pending = 0;
    provisional = 1;   /* park-and-listen until the first poll is caught */
    bond_save_pending = 1;
    rf_state = RF_STATE_CONNECTED;

#if RF_DIAG_COUNTERS
    ll_trace_count = 0;
    ll_resp_suppressed = 0;
    ll_trace_armed = 1;   /* capture the first LL_TRACE_N connected callbacks */
#endif
    /* No active TMR0 here: the listen channel is advanced by elapsed RTC time in
     * RF_ConnectedTick(), polled from the main loop (stock 0x20000E06 model). */
    rf_tmr0_stop();
    rf_start_rx(data_channels[prev_data_idx]);
    PFIC_EnableIRQ(TMR0_IRQn);   /* TMR0 = post-poll response turnaround timer */
    rf_stop_task_atomic(RF_EVT_DISCONNECT);   /* reset, don't double-arm a stale timer */
    rf_start_task_atomic(RF_EVT_DISCONNECT, RF_CONNECTED_TIMEOUT_TICKS);

    KeyboardUart_SendStatus(0x32);
    KeyboardUart_SendStatus(0x23);
    KeyboardUart_SendLed(0x00);
    last_led_sent = 0x00;
}

/* Time-based connected-mode hop (stock firmwareB.bin 0x20000E06). Polled from
 * the main loop every iteration. When at least one conn-interval of RTC32K time
 * has elapsed since the anchor, advance the LISTEN channel by elapsed/interval
 * data channels and retune RX. Because the channel is driven by ELAPSED TIME
 * (not by catching a poll), it keeps following the dongle's hop through missed
 * polls; the PLL servo in the RX path keeps it phase-locked. */
__HIGH_CODE
void RF_ConnectedTick(void)
{
    uint32_t now;
    uint32_t elapsed;
    uint16_t interval;
    uint32_t step;
    uint32_t irq;
    uint8_t  advanced = 0;

    if (rf_state != RF_STATE_CONNECTED) {
        return;
    }
    RF_DIAG_INC(connected_tick_calls);   /* hop code past the state gate actually ran */

    /* Re-arm the connection-supervision timer here (main-loop context) when the
     * RX ISR flagged an incoming poll. Placed BEFORE the response_pending
     * early-return so supervision always resets promptly. This is the deferred
     * counterpart to the old ISR-context tmos_start/stop_task (the HardFault fix). */
    if (supervision_kick) {
        supervision_kick = 0;
        rf_stop_task_atomic(RF_EVT_DISCONNECT);
        rf_start_task_atomic(RF_EVT_DISCONNECT, RF_CONNECTED_TIMEOUT_TICKS);
    }

    interval = rf_conn_interval ? rf_conn_interval : RF_PAIR_INTERVAL;

    /* Don't retune/shut the radio while a response is pending (the latched TX
     * owns the radio until TX_FINISH) -- BUT a lost/late TX_FINISH must NOT stall
     * the hop, or the keyboard freezes on one channel and loses lock. Force-
     * release after 2 intervals so the hop keeps following and re-acquires. */
    if (response_pending) {
        if (rtc_delta_32k(rtc_read_32k(), response_armed_rtc) <=
            (uint32_t)interval * 2u) {
            return;
        }
        response_pending = 0;
    }

    /* Critical section (stock runs its hop scheduler with IRQ save/restore): the
     * RX-ISR servo and this advance both read-modify-write hop_anchor/since_rx,
     * so disable IRQs while we touch them. Keep rf_start_rx() OUT of the critical
     * section (stock defers the RX arm to event 0x04). */
    irq = rf_irq_save();
    now = rtc_read_32k();
    elapsed = rtc_delta_32k(now, hop_anchor);
    if (provisional) {
        /* PARK on the seed channel: keep prev_data_idx fixed, so we dwell on one
         * channel and the dongle's sweep lands on us. The default build re-arms
         * RX once per interval; STOCK_CONTINUOUS_PARK_ACQ keeps RX continuously
         * open to test whether those re-arm gaps hide stock polls. */
        if (elapsed > interval) {
            hop_anchor = rtc_add_32k(hop_anchor, interval);
#if !STOCK_CONTINUOUS_PARK_ACQ
            advanced = 1;
#endif
        }
    } else if (elapsed > (uint32_t)interval * RF_HOP_DESYNC_INTERVALS) {
        /* Desync safety-net (rare): a >64-interval gap -- the anchor raced ahead,
         * or RF_ConnectedTick did not run for tens of ms. Re-seed the phase to
         * now-backdate so the hop stops free-running. This corrects PHASE only,
         * NOT the channel index: the RX servo nudges hop_anchor and clears
         * provisional but never re-writes prev_data_idx (only the normal hop
         * branch advances it). It also cannot recover a main-loop stall -- by the
         * time a >64-interval (~56 ms) gap accrues, the keyboard has been silent
         * long enough that the dongle's own ~26-poll (~22 ms) supervision has
         * already dropped the link, so recovery is the IDLE->reconnect path, not
         * this branch. (Park-and-snap here -- provisional=1 -- was tried on the
         * bench and measured to add nothing, for exactly that reason.) */
        hop_anchor = rtc_backdate_32k(now, RF_HOP_ANCHOR_BACKDATE);
        RF_DIAG_INC(ll_desync_reseed);
        advanced = 1;   /* re-arm RX on the current channel so we can re-catch */
    } else if (elapsed > interval) {
        step = elapsed / interval;
        if (step > RF_HOP_MAX_STEP) {
            step = RF_HOP_MAX_STEP;   /* clamp a stale anchor; the servo re-locks the phase */
            RF_DIAG_INC(ll_step_clamp);
        }
        hop_anchor = rtc_add_32k(hop_anchor, (uint32_t)interval * step);
        prev_data_idx = (uint8_t)((prev_data_idx + step) % NUM_DATA_CHANNELS);
        /* rf_channel is committed in rf_start_rx() below (R1): keep it equal to
         * the radio's armed channel so the RX-ISR response_channel latch is correct */
        /* stock one-tick rollback on the slot immediately after an RX */
        if (step == 1u && since_rx == 1u) {
            hop_anchor = rtc_backdate_32k(hop_anchor, 1);
        }
        /* Keep drop diagnostics monotonic across long droughts; supervision
         * normally fires well before this saturates. */
        if (since_rx < 0xffffu) {
            since_rx++;
        }
#if RF_DIAG_COUNTERS
        if (since_rx > ll_since_rx_max) ll_since_rx_max = since_rx;
#endif
        advanced = 1;
    }
    rf_irq_restore(irq);

    if (advanced) {
        RF_DIAG_INC(hop_advance_count);   /* the RTC-timed hop actually retuned */
        if (response_pending) {
            return;
        }
        rf_start_rx(data_channels[prev_data_idx]);
    }
}

static void rf_tmr0_stop(void)
{
    PFIC_DisableIRQ(TMR0_IRQn);
    R8_TMR0_CTRL_MOD = 0;
    R8_TMR0_INTER_EN = 0;
    R8_TMR0_INT_FLAG = RB_TMR_IF_CYC_END;
    PFIC_ClearPendingIRQ(TMR0_IRQn);
}

/* Arm TMR0 as a one-shot: count to `count`, fire the cycle-end IRQ once. The IRQ
 * handler stops it (so it does not free-run) and posts the response event. Called
 * from the RX callback (ISR context) — register writes only. */
__HIGH_CODE
static void rf_tmr0_arm(uint32_t count)
{
    R8_TMR0_INTER_EN = 0;
    R32_TMR0_CNT_END = count;
    R8_TMR0_CTRL_MOD = RB_TMR_ALL_CLEAR;
    R8_TMR0_INT_FLAG = RB_TMR_IF_CYC_END;
    R8_TMR0_INTER_EN = RB_TMR_IF_CYC_END;
    R8_TMR0_CTRL_MOD = RB_TMR_COUNT_EN;
}

/* Post-poll turnaround timer (stock TMR0 IRQ at firmwareB.bin ~0x20001446):
 * stop the one-shot, then post the response event so the main loop TXes inside
 * the dongle's post-poll RX window. */
__INTERRUPT
__HIGH_CODE
void TMR0_IRQHandler(void)
{
    RF_DIAG_INC(tmr0_irq_count);
    if ((R8_TMR0_INT_FLAG & RB_TMR_IF_CYC_END) == 0) {
        return;
    }
    R8_TMR0_INT_FLAG = RB_TMR_IF_CYC_END;
    R8_TMR0_CTRL_MOD = 0;        /* one-shot: stop so it fires once per arm */
    if (rf_state == RF_STATE_CONNECTED) {
#if STOCK_ISR_FAST_RESPONSE
        rf_do_response_tx();     /* TX here (~5us post-poll) instead of ~275us via the main loop */
#else
        rf_set_event_atomic(RF_EVT_RESPOND);
#endif
    }
}

void RF_TaskInit(void)
{
#if KBD_RF_CRYPT
    /* Before any bond load: rf_load_bond_from_flash() installs the link key
     * through this backend. Safe here because CH59x_BLEInit()/RF_RoleInit()
     * have already run by the time the RF task is created. */
    kbd_crypt_init();
    /* .diag_safe is NOLOAD and deliberately not zeroed by startup (that is how
     * ll_boot_count survives a reset), so these must be cleared explicitly or
     * they read as stale RAM. */
    kbd_crypt_sess_rx = 0;
    kbd_crypt_sess_ok = 0;
    kbd_crypt_sess_bad = 0;
    kbd_crypt_tx_sealed = 0;
    kbd_crypt_seal_miss = 0;
#endif
#if RF_DIAG_COUNTERS
    for (uint8_t i = 0; i < 6; i++) {
        rf_cb_count[i] = 0;
    }
    rf_pair_bcast_count = 0;
    rf_connected_tx_count = 0;
    rf_valid_rx_count = 0;
    rf_last_config_status = 0;
    rf_last_rx_status = 0;
    rf_last_tx_status = 0;
    tmr0_irq_count = 0;
    entered_connected_count = 0;
    connected_tick_calls = 0;
    hop_advance_count = 0;
    rf_config_count = 0;
    last_rf_op = 0;
    last_cb_state = 0;
    last_cb_sta = 0;
    cb_in_cs = 0;
    bb_cb_in_cs_count = 0;
    for (uint8_t i = 0; i < 5; i++) ll_kbd_rx_ch[i] = 0;
    ll_since_rx_max = 0; ll_desync_reseed = 0; ll_step_clamp = 0;
    ll_servo_plus = 0; ll_servo_minus = 0; ll_servo_noop = 0;
    ll_drop_count = 0; ll_drop1_rtc = 0; ll_dropN_rtc = 0;
    ll_drop1_since_rx = 0; ll_drop1_idx = 0; ll_drop1_channel = 0; ll_drop1_provis = 0;
    ll_dropN_since_rx = 0; ll_dropN_idx = 0; ll_dropN_channel = 0; ll_dropN_provis = 0;
    ll_trace_count = 0; ll_trace_armed = 0; ll_resp_suppressed = 0;
    ll_rx_rearm_count = 0; ll_rx_rearm_gap_last = 0; ll_rx_rearm_gap_min = 0;
    ll_rx_rearm_gap_max = 0; ll_rx_rearm_gap_sum = 0;
    ll_rx_rearm_begin_sys = 0; ll_rx_rearm_end_sys = 0;
    ll_rx_cb_last_sys = 0; ll_rx_since_rearm_sys = 0;
    ll_rx_rearm_last_idx = 0; ll_rx_rearm_last_channel = 0;
    ll_rx_rearm_last_provis = 0;
    ll_boot_count++;   /* NOT zeroed: counts RF_TaskInit runs to detect reboots mid-test */
#endif

    rf_taskID = TMOS_ProcessEventRegister(RF_ProcessEvent);
    if (rf_taskID == INVALID_TASK_ID) {
        while (1) {
        }
    }
    rf_state = RF_STATE_IDLE;
    rf_access_addr = RF_DEFAULT_ACCESS_ADDR;
    rf_configured_aa = 0;   /* force RF_Config on first RX/TX */
    rf_channel = pair_channels[0];
    rf_conn_interval = RF_PAIR_INTERVAL;
    pending_led = 0;
    last_led_sent = 0xFF;
    bond_save_pending = 0;
    rf_clear_bond_ram();
#if STOCK_SEED_BOND
    has_bond = 1;
    stored_session_aa = STOCK_SEED_SESSION_AA;
    stored_type_tag = (uint8_t)STOCK_SEED_TYPE_TAG;
    stored_dongle_mac[0] = STOCK_SEED_DONGLE_MAC_0;
    stored_dongle_mac[1] = STOCK_SEED_DONGLE_MAC_1;
    stored_dongle_mac[2] = STOCK_SEED_DONGLE_MAC_2;
    stored_dongle_mac[3] = STOCK_SEED_DONGLE_MAC_3;
    stored_dongle_mac[4] = STOCK_SEED_DONGLE_MAC_4;
    stored_dongle_mac[5] = STOCK_SEED_DONGLE_MAC_5;
#else
    (void)rf_load_bond_from_flash();
#endif
    tmos_memset(hid_report, 0, sizeof(hid_report));
    if (has_bond) {
        rf_state = RF_STATE_PAIRING;
        rf_access_addr = stored_session_aa;
        rf_channel = pair_channels[0];
        pair_bcast_count = 0;
        rf_set_event_atomic(RF_EVT_PAIR_BCAST);
    } else {
        rf_set_event_atomic(RF_EVT_START);
    }
}

uint8_t RF_Select2G4(void)
{
    if (rf_state == RF_STATE_CONNECTED) {
        return has_bond;
    }

    if (has_bond) {
        rf_tmr0_stop();
        rf_state = RF_STATE_PAIRING;
        rf_access_addr = stored_session_aa;
        rf_channel = pair_channels[0];
        pair_bcast_count = 0;
        rf_stop_task_atomic(RF_EVT_PAIR_TIMEOUT);
        rf_set_event_atomic(RF_EVT_PAIR_BCAST);
    }
    return has_bond;
}

void RF_EnterPairing(void)
{
    /* A6 51 ("pair current transport") must NOT tear down a healthy link. A host
     * that reconnects with A6 30 + A6 51, or sends a stray/late A6 51 after a
     * bonded reconnect already entered CONNECTED, would otherwise kick us back to
     * default-AA pairing (rf_access_addr -> RF_DEFAULT_ACCESS_ADDR) while the
     * dongle keeps polling the session AA -> a silent phantom (the natural-drop
     * reconnect failure). Forcing a fresh pair while connected requires an
     * explicit unpair (A6 52) or disconnect first. */
    if (rf_state == RF_STATE_CONNECTED) {
        return;
    }
    rf_tmr0_stop();
    rf_state = RF_STATE_PAIRING;
    rf_access_addr = RF_DEFAULT_ACCESS_ADDR;
    rf_channel = pair_channels[0];
    pair_bcast_count = 0;
    rf_set_event_atomic(RF_EVT_PAIR_BCAST);
    rf_start_task_atomic(RF_EVT_PAIR_TIMEOUT, RF_PAIR_WINDOW_TICKS);
}

void RF_Disconnect(void)
{
    rf_tmr0_stop();
    rf_stop_task_atomic(RF_EVT_PAIR_BCAST);
    rf_stop_task_atomic(RF_EVT_PAIR_TIMEOUT);
    rf_stop_task_atomic(RF_EVT_DISCONNECT);
    bond_save_pending = 0;
    rf_state = RF_STATE_IDLE;
    RF_Shut();
}

/* Perform the deferred bond save NOW if one is pending. Normally the save
 * runs as a TMOS event posted on the first caught connected poll; a
 * firmware-update request landing in that window would otherwise lose the
 * fresh bond, because RF_Disconnect() clears bond_save_pending. The write is
 * done SYNCHRONOUSLY here rather than by posting RF_EVT_SAVE_BOND: TMOS
 * services one event per scheduler pass, so a posted event can still be
 * queued - and lost - when the update path resets the chip. Main-loop
 * context only (same context the TMOS handler runs in). */
void RF_FlushBondSave(void)
{
    if (bond_save_pending) {
        bond_save_pending = 0;
        rf_save_bond_to_flash();
    }
}

void RF_ClearBond(void)
{
    rf_clear_bond_ram();
    bond_save_pending = 0;
    rf_clear_bond_flash();
}

uint8_t RF_HasBond(void)
{
    return has_bond;
}

void RF_QueueHIDReport(const uint8_t report[8])
{
    uint8_t changed = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (hid_report[i] != report[i]) {
            changed = 1;
        }
        hid_report[i] = report[i];
    }
    /* A new report (key event) must be delivered as LEN=10. Resend it on the
     * next few polls so a dropped slot doesn't lose a key-down or key-up. */
    if (changed) {
        hid_resend = HID_RESEND_COUNT;
#if KBD_RF_CRYPT
        /* Only DISCARD the prepared frame here -- do not build its replacement.
         *
         * Any prepared frame holds the previous report, so it must not go out.
         * But this runs from the UART command handler, which is asynchronous to
         * the radio's poll grid, and sealing costs ~160 us of AES. With the
         * default STOCK_ISR_FAST_RESPONSE=0 the turnaround ISR only posts
         * RF_EVT_RESPOND, and this single cooperative main loop cannot service
         * it until the seal returns -- so a seal starting shortly before a poll
         * pushes the response past the receiver's post-poll RX window and the
         * response is simply lost. Bench-observed: a keystroke that never
         * arrived while the link was otherwise healthy, and a keepalive rate at
         * roughly half its nominal value.
         *
         * Discarding is cipher-free and safe anywhere. The replacement is built
         * by the RF_EVT_CRYPT_ARM handler, which is posted from the response
         * path -- the quiet window immediately after a transmission, ~700 us
         * before the next poll. The report therefore waits one extra poll
         * (~1.75 ms rather than ~875 us) before going on air, which is far below
         * anything a typist can perceive and much cheaper than losing it. */
        kbd_crypt_seal_discard();
#endif
    }
}

#if KBD_RF_CRYPT
/* Provision the 16-byte link key into the bond and activate encryption.
 *
 * BRING-UP SCAFFOLD. Until the key-establishment handshake exists, both ends
 * are given the same key out of band -- this is the keyboard half, the
 * receiver's is its USB IAP BondWrite. It is deliberately not reachable from
 * any shipped command path: a link key that any host can overwrite at will is
 * an open door to exactly the injection this feature exists to stop.
 *
 * Returns 0 if there is no bond to attach the key to, or if the key is one of
 * the two values that mean "erased flash" rather than a key. */
uint8_t RF_ProvisionLinkKey(const uint8_t key[KBD_CRYPT_KEY_BYTES])
{
    uint8_t all_zero = 1;
    uint8_t all_ff = 1;

    if (!has_bond) {
        return 0;
    }
    for (uint8_t i = 0; i < KBD_CRYPT_KEY_BYTES; i++) {
        if (key[i] != 0x00u) {
            all_zero = 0;
        }
        if (key[i] != 0xFFu) {
            all_ff = 0;
        }
    }
    if (all_zero || all_ff) {
        return 0;
    }

    for (uint8_t i = 0; i < KBD_CRYPT_KEY_BYTES; i++) {
        stored_link_key[i] = key[i];
    }
    stored_bond_flags |= (uint8_t)(RF_BOND_FLAG_ENC_KEY | RF_BOND_FLAG_ENC_CAPABLE);
    kbd_crypt_install_key(stored_link_key, rf_crypt_ctr_start());
    rf_save_bond_to_flash();
    return 1;
}
#endif

uint8_t RF_GetState(void)
{
    return rf_state;
}

int8_t RF_GetRSSI(void)
{
    return rf_rssi;
}
