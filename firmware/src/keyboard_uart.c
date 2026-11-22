/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 */
#include "CONFIG.h"
#include "HAL.h"
#include "keyboard_uart.h"
#include "rtc_clock.h"

#if KBD_KEX
#include "kex_proto.h"       /* KEX_SEED_*: the entropy seed packet */
#define KBD_UART_MAX_FRAME  KEX_SEED_PACKET
#else
#define KBD_UART_MAX_FRAME  24
#endif

#ifndef KBD_UART1_REMAP
#define KBD_UART1_REMAP 1
#endif

/* Bound the TX-FIFO-full spin: a stalled host main-MCU must not freeze
 * Main_Circulation (which would stall RF_ConnectedTick -> the time-based hop ->
 * drop the RF link with no recovery). One byte is ~87 us at 115200; 150 us lets a
 * normally-draining full FIFO free a slot but is far below the ~0.85 ms hop
 * interval. On timeout we abort the rest of the frame (lose one ACK/status frame
 * -- already a known gap) and keep RF alive. */
#define KBD_UART_TX_WAIT_US  150u

#ifndef RF_DIAG_COUNTERS
#define RF_DIAG_COUNTERS 0
#endif
#ifndef KBD_UART_DIAG_COUNTERS
#define KBD_UART_DIAG_COUNTERS RF_DIAG_COUNTERS
#endif

#if KBD_HOST_WAKE
#if KBD_UART1_REMAP
#error "KBD_HOST_WAKE drives PB13, which the UART1 remap uses as TXD1"
#endif
#include "host_wake.h"
/* PB13: TXD1 under the UART1 remap, the CHWAKE line on the MK65MX. */
#define KBD_CHWAKE_PIN bTXD1_
static host_wake_t host_wake;
static uint8_t chwake_level;

#endif

static keyboard_uart_frame_cb_t frame_cb;
static uint8_t rx_buf[KBD_UART_MAX_FRAME];
static uint8_t rx_len;
static uint8_t rx_expected;

/* Raw RX activity latch (power ladder MR7): set on EVERY byte taken from the
 * hardware -- including bytes the parser later discards (a stray NULL wake
 * preamble, line noise, a host 61 0D 0A ACK), none of which reach the frame
 * callback. The auto-sleep holdoff consumes it, so a NULL that lands while the
 * module is awake still counts as "the host is talking" and defers sleep
 * (review finding: otherwise the real frame's first byte becomes the lost
 * wake byte). Set in ISR/polled context, consumed in main-loop context. */
#ifndef KBD_DEEP_SLEEP
#define KBD_DEEP_SLEEP 0
#endif
#if KBD_DEEP_SLEEP
static volatile uint8_t rx_activity_latch;
#define RX_NOTE_ACTIVITY()  do { rx_activity_latch = 1; } while (0)
#else
#define RX_NOTE_ACTIVITY()  do { } while (0)   /* KBD_DEEP_SLEEP=0: byte-identical to MR4 */
#endif

#if KBD_IDLE_WFI
/* RX ring between UART1_IRQHandler and KeyboardUart_Poll. The ISR exists so
 * the WFE idle wait in Main_Circulation ends the instant a host byte arrives
 * (RECV_RDY pending is the wake event); it MUST drain RBR because RECV_RDY
 * clears only by reading data - an
 * ack-only handler would retrigger forever. Single-producer/single-consumer:
 * head is ISR-owned, tail main-loop-owned, uint8_t indexes wrap mod 256 and
 * are masked mod size on access, so no index ever needs a critical section. */
#if KBD_KEX
#define KBD_UART_RING_SIZE 128u  /* power of two, > 2 max frames (the 35-byte seed) */
#else
#define KBD_UART_RING_SIZE 64u   /* power of two, > 2 max frames */
#endif
static volatile uint8_t rx_ring[KBD_UART_RING_SIZE];
static volatile uint8_t rx_ring_head;
static volatile uint8_t rx_ring_tail;
/* Set by the ISR on a hardware overrun OR a full ring (same recovery: the
 * byte stream has a hole, so the parser must resync). Consumed by Poll.
 * A fast build (KBD_UART_FAST) sets it on a framing error or a break too,
 * and wherever else LSR is read, so kbd_uart_rx_overrun_count counts those
 * recoveries as well. */
static volatile uint8_t rx_overrun_latch;
#if KBD_UART_FAST
/* Reading LSR clears these, so every read of it has to keep them. */
#define KBD_UART_LSR_RX_ERR (RB_LSR_OVER_ERR | RB_LSR_FRAME_ERR | RB_LSR_BREAK_ERR)
#endif
#endif

#if KBD_UART_DIAG_COUNTERS
volatile uint32_t kbd_uart_rx_overrun_count __attribute__((section(".diag_safe")));
volatile uint32_t kbd_uart_rx_frame_error_count __attribute__((section(".diag_safe")));
volatile uint32_t kbd_uart_rx_resync_count __attribute__((section(".diag_safe")));
#define KBD_UART_DIAG_INC(x) do { (x)++; } while (0)
#else
#define KBD_UART_DIAG_INC(x) do { } while (0)
#endif

static uint8_t checksum(const uint8_t *buf, uint8_t len)
{
    uint8_t sum = 0;
    for (uint8_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    return sum;
}

static uint8_t uart_send_byte(uint8_t b)
{
    /* Timed on RTC32K (rtc_clock.h): SysTick was found stopped in the running
     * application, which left this bound nominal. RTC rounding makes it 150 us
     * to about 190 us. The clock is only read once the FIFO is full, so the
     * common case costs nothing. */
    if (R8_UART1_TFC == UART_FIFO_SIZE) {
        uint32_t start = RtcClock_Now();

        while (R8_UART1_TFC == UART_FIFO_SIZE) {
            if ((uint32_t)(RtcClock_Now() - start)
                    >= RTC_CLOCK_TICKS_US(KBD_UART_TX_WAIT_US)) {
                return 0;   /* host not draining -> abort, don't freeze the loop */
            }
        }
    }
    R8_UART1_THR = b;
    return 1;
}

static uint8_t uart_send_frame(uint8_t cmd, uint8_t val)
{
    if (!uart_send_byte(cmd)) {
        return 0;
    }
    if (!uart_send_byte(val)) {
        return 0;
    }
    return uart_send_byte((uint8_t)(cmd + val));
}

void KeyboardUart_Init(void)
{
#if KBD_UART1_REMAP
    GPIOPinRemap(ENABLE, RB_PIN_UART1);
    GPIOB_SetBits(bTXD1_);
    GPIOB_ModeCfg(bTXD1_, GPIO_ModeOut_PP_5mA);
    GPIOB_ModeCfg(bRXD1_, GPIO_ModeIN_PU);
#else
    /* MK65MX uses UART1's default PA8/PA9 mapping.  PB13 is CHWAKE on that
     * board, pulled down at the host (STM32 PA1).  A KBD_HOST_WAKE build
     * drives it push-pull from here on, low until a frame is queued, so a
     * reset module never leaves it unexplained; any other build leaves it a
     * FLOATING input, since a pull-up against the host's pull-down would
     * burn current for nothing.  PB12 is genuinely unconnected there and
     * keeps the input-pull-up park from main(); floating it leaves a CMOS
     * input mid-rail burning crossbar current. */
#if KBD_HOST_WAKE
    GPIOB_ResetBits(KBD_CHWAKE_PIN);
    GPIOB_ModeCfg(KBD_CHWAKE_PIN, GPIO_ModeOut_PP_5mA);
    chwake_level = 0;
    HostWake_Init(&host_wake, RTC_CLOCK_TICKS_US(KBD_HOST_WAKE_GUARD_US),
                  RTC_CLOCK_TICKS_US(KBD_HOST_WAKE_ACK_TIMEOUT_US),
                  KBD_HOST_WAKE_ATTEMPTS);
#else
    GPIOB_ModeCfg(bTXD1_, GPIO_ModeIN_Floating);
#endif
    GPIOA_SetBits(bTXD1);
    GPIOA_ModeCfg(bTXD1, GPIO_ModeOut_PP_5mA);
    GPIOA_ModeCfg(bRXD1, GPIO_ModeIN_PU);
    /* Switch the peripheral only after PA9 is already idling high, avoiding a
     * transient low start edge if this follows firmware that used the remap. */
    GPIOPinRemap(DISABLE, RB_PIN_UART1);
#endif

    UART1_DefInit();
    UART1_BaudRateCfg(KBD_UART_BAUD);
    UART1_CLR_RXFIFO();
    UART1_CLR_TXFIFO();

#if KBD_IDLE_WFI
    /* 1-byte trigger so the first byte of a frame ends a WFI immediately;
     * LINE_STAT keeps the overrun accounting the polled path had. */
    UART1_ByteTrigCfg(UART_1BYTE_TRIG);
    UART1_INTCfg(ENABLE, RB_IER_RECV_RDY | RB_IER_LINE_STAT);
    PFIC_EnableIRQ(UART1_IRQn);
#endif
}

void KeyboardUart_SetFrameCallback(keyboard_uart_frame_cb_t cb)
{
    frame_cb = cb;
}

void KeyboardUart_SendAck(void)
{
    if (!uart_send_byte(0x61)) {
        return;
    }
    if (!uart_send_byte(0x0D)) {
        return;
    }
    uart_send_byte(0x0A);
}

/* Every frame the module originates goes through here. With host wake it is
 * queued behind CHWAKE and sent by KeyboardUart_Service(). */
static void send_host_frame(uint8_t cmd, uint8_t val)
{
#if KBD_HOST_WAKE
    HostWake_Enqueue(&host_wake, cmd, val);   /* full: dropped and counted */
#else
    uart_send_frame(cmd, val);
#endif
}

void KeyboardUart_SendStatus(uint8_t sub)
{
    send_host_frame(0x5B, sub);
}

void KeyboardUart_SendBattery(uint8_t percent)
{
    send_host_frame(0x5C, percent);
}

void KeyboardUart_SendLed(uint8_t led_mask)
{
#if KBD_HOST_WAKE
    /* Only the newest LED state matters, and rf_task.c records it as sent the
     * moment it is handed over: it goes into a slot that cannot overflow. */
    HostWake_SetLatest(&host_wake, 0x5A, led_mask);
#else
    uart_send_frame(0x5A, led_mask);
#endif
}

void KeyboardUart_Service(void)
{
#if KBD_HOST_WAKE
    host_wake_step_t step = HostWake_Step(&host_wake, RtcClock_Now());

    if (step.line != chwake_level) {
        if (step.line) {
            GPIOB_SetBits(KBD_CHWAKE_PIN);
        } else {
            GPIOB_ResetBits(KBD_CHWAKE_PIN);
        }
        chwake_level = step.line;
    }
    if (step.send) {
        /* Bounded like every send: a host that is not draining loses this
         * copy, which the missing ACK then retries. */
        uart_send_frame(step.cmd, step.val);
    }
#endif
}

uint8_t KeyboardUart_TakeLedLost(void)
{
#if KBD_HOST_WAKE
    uint8_t lost = host_wake.latest_lost;

    host_wake.latest_lost = 0;
    return lost;
#else
    return 0;
#endif
}

uint8_t KeyboardUart_HostWakeIdle(void)
{
#if KBD_HOST_WAKE
    return HostWake_Idle(&host_wake);
#else
    return 1;
#endif
}

uint8_t KeyboardUart_SendRaw(const uint8_t *buf, uint8_t len)
{
    uint8_t i;
    for (i = 0; i < len; i++) {
        if (!uart_send_byte(buf[i])) {
            break;              /* bounded: a non-draining host aborts the frame */
        }
    }
    return i;
}

#if KBD_CRYPT_BENCH_KEY
void KeyboardUart_SendCryptFail(uint8_t latched, uint8_t len, uint32_t session,
                                uint8_t seal_bb, const uint8_t *frame22,
                                const uint8_t *good8, const uint8_t *plain8,
                                const uint8_t *s1_8, const uint8_t *s0_8)
{
    uint8_t hdr[8];
    uint8_t chk = 0;
    uint8_t i;
    const uint8_t *vecs[4];
    uint8_t v;

    vecs[0] = good8;
    vecs[1] = plain8;
    vecs[2] = s1_8;
    vecs[3] = s0_8;

    hdr[0] = 0x5F;
    hdr[1] = latched;
    hdr[2] = len;
    hdr[3] = (uint8_t)session;
    hdr[4] = (uint8_t)(session >> 8);
    hdr[5] = (uint8_t)(session >> 16);
    hdr[6] = (uint8_t)(session >> 24);
    hdr[7] = seal_bb;
    for (i = 0; i < 8u; i++) {
        if (!uart_send_byte(hdr[i])) {
            return;
        }
        chk = (uint8_t)(chk + hdr[i]);
    }
    for (i = 0; i < 22u; i++) {
        if (!uart_send_byte(frame22[i])) {
            return;
        }
        chk = (uint8_t)(chk + frame22[i]);
    }
    for (v = 0; v < 4u; v++) {
        for (i = 0; i < 8u; i++) {
            if (!uart_send_byte(vecs[v][i])) {
                return;
            }
            chk = (uint8_t)(chk + vecs[v][i]);
        }
    }
    uart_send_byte(chk);
}

#endif
#if KBD_CRYPT_BENCH_KEY || KBD_TX_OUTCOME
/* [lead][n x u32 LE][chk], chk seeded WITH the lead byte (so the host can just
 * sum the whole frame except its last byte). Shared by every u32-vector reply
 * so a second one cannot drift from the first's checksum rule. */
static void uart_send_u32_frame(uint8_t lead, const uint32_t *v, uint8_t n)
{
    uint8_t chk = lead;
    uint8_t i, b;

    if (!uart_send_byte(lead)) {
        return;
    }
    for (i = 0; i < n; i++) {
        uint8_t k;
        for (k = 0; k < 4u; k++) {
            b = (uint8_t)(v[i] >> (8u * k));
            if (!uart_send_byte(b)) {
                return;   /* TX wedged: abandon rather than desync the checksum */
            }
            chk = (uint8_t)(chk + b);
        }
    }
    uart_send_byte(chk);
}

#endif
#if KBD_CRYPT_BENCH_KEY
void KeyboardUart_SendCryptDiag(const uint32_t *counters, uint8_t n)
{
    uart_send_u32_frame(0x5Du, counters, n);
}
#endif

#if KBD_TX_OUTCOME
void KeyboardUart_SendTxOutcome(const uint32_t *counters, uint8_t n)
{
    uart_send_u32_frame(0x62u, counters, n);
}
#endif

static void reset_parser(void)
{
    rx_len = 0;
    rx_expected = 0;
}

/* A frame left incomplete this long is abandoned. A host sends every frame
 * in one piece (35 bytes take 3 ms at 115200 baud), so a parser still waiting
 * after this has locked onto a stray header byte, and would otherwise hold
 * the bytes it has for ever: KeyboardUart_RxQuiet() stays false and the
 * module never idles. Matches the host's own partial-frame timeout. */
#define KBD_UART_RX_PARTIAL_US 20000u
static uint32_t rx_last_byte_at;      /* RtcClock_Now() at the last byte fed */

#if KBD_UART_FAST
/* After a hole in the stream, a fast build parses nothing until the line
 * has been silent this long. Resetting the parser is not enough: what
 * arrives next may be the rest of the frame the hole was in, and a seed's
 * body is 32 arbitrary bytes, which can hold a well-formed command (A6 52
 * F8 is unpair). Silence is a frame boundary because a host sends each
 * frame in one piece (35 bytes take 0.7 ms here), and a frame dropped here
 * is not ACKed, so the host says nothing more for its 20 ms retry interval
 * apart from ACKs of its own, which are three bytes.
 *
 * It has to stay well under two things: that retry interval, and the 5 ms
 * a host leaves between its wake byte and the frame behind it, since the
 * wake byte can arrive damaged while the clock restarts. A frame that
 * starts inside the wait is dropped whole, and a host that retries (the
 * keyboard does; the bench `make update` helper sends its command three
 * times for this reason) gets through on the next attempt.
 *
 * The time is taken when Poll discards a byte, not when the byte arrived,
 * so a late Poll lengthens the wait and never shortens it. */
#define KBD_UART_RX_RESYNC_US 1000u
static uint8_t  rx_resync;            /* discarding until the line is silent */
static uint32_t rx_resync_at;         /* RtcClock_Now() at the last discard */
#endif

static void wipe_rx_buf(void)
{
    volatile uint8_t *p = rx_buf;

    for (uint8_t i = 0; i < KBD_UART_MAX_FRAME; i++) {
        p[i] = 0;
    }
}

/* Call from KeyboardUart_Poll() once the bytes that have arrived are fed. */
static void expire_partial_frame(uint8_t fed)
{
    if (rx_len == 0) {
        return;
    }
    if (fed) {
        rx_last_byte_at = RtcClock_Now();
    } else if ((uint32_t)(RtcClock_Now() - rx_last_byte_at)
                   >= RTC_CLOCK_TICKS_US(KBD_UART_RX_PARTIAL_US)) {
        KBD_UART_DIAG_INC(kbd_uart_rx_frame_error_count);
        wipe_rx_buf();      /* it may be part of an entropy seed */
        reset_parser();
    }
}

static uint8_t expected_for_header(uint8_t b)
{
    switch (b) {
    case 0xA6:
    case 0x61:
#if KBD_BLE
    case 0xA7:  /* battery level for the BLE Battery Service */
    case 0xA4:  /* system control bitmap */
#endif
        return 3;
    case 0xA3:  /* consumer usage, 16-bit little endian */
        return 4;
    case 0xA8:  /* mouse report: sequence, buttons, X, Y, wheel, pan (sent only after 5B 3B) */
        return 8;
    case 0xA1:
        return 10;
#if KBD_CRYPT_BENCH_KEY
    case KBD_UART_CMD_SET_LINK_KEY:
        return 18;   /* [AE][key 0..15][chk] */
    case KBD_UART_CMD_CRYPT_DIAG:
        return 2;    /* [AF][chk] -- read-only, no body */
    case KBD_UART_CMD_CRYPT_FAIL:
        return 2;    /* [B0][chk] -- read-only, no body */
    case KBD_UART_CMD_CRYPT_VERIFY:
        return 3;    /* [B1][mode][chk] */
#endif
#if KBD_TX_OUTCOME
    case KBD_UART_CMD_TX_OUTCOME:
        return 2;    /* [B2][chk] -- read-only, no body */
#endif
#if KBD_KEX
    case KEX_SEED_CMD:
        /* [9C][20][seed 0..31][chk]: the packet the dongle takes over USB,
         * byte for byte (kex_proto.h). The host sends it only after 5B 3A
         * has answered its A6 5A. */
        return KEX_SEED_PACKET;
#endif
    case 0xA9:
        /* BLE device-name frames are FIXED 21 bytes on the wire: the host
         * pads [A9][len][name...][chk] out to 21 regardless of len (stock
         * host behavior, bench-validated). The checksum sits at 2+len
         * inside the padded frame — see frame_is_valid(). Do not switch
         * this to len-derived framing; unread padding would desync the
         * parser against real hosts. */
        return 21;
    default:
        return 0;
    }
}

static uint8_t frame_is_valid(void)
{
    uint8_t cmd = rx_buf[0];

    if (cmd == 0xA6) {
        return checksum(rx_buf, 2) == rx_buf[2];
#if KBD_BLE
    } else if (cmd == 0xA7 || cmd == 0xA4) {
        return checksum(rx_buf, 2) == rx_buf[2];
#endif
    } else if (cmd == 0xA3) {
        return checksum(rx_buf, 3) == rx_buf[3];
    } else if (cmd == 0xA8) {
        return checksum(rx_buf, 7) == rx_buf[7];
    } else if (cmd == 0xA1) {
        return checksum(rx_buf, 9) == rx_buf[9];
    } else if (cmd == 0xA9) {
        uint8_t name_len = rx_buf[1];
        uint8_t chk_idx = (uint8_t)(2 + name_len);
        return (name_len <= 18 && chk_idx < 21 &&
                checksum(rx_buf, chk_idx) == rx_buf[chk_idx]);
    } else if (cmd == 0x61) {
        return rx_buf[1] == 0x0D && rx_buf[2] == 0x0A;
#if KBD_KEX
    } else if (cmd == KEX_SEED_CMD) {
        return rx_buf[1] == KEX_SEED_BYTES &&
               checksum(rx_buf, KEX_SEED_PACKET - 1u) == rx_buf[KEX_SEED_PACKET - 1u];
#endif
#if KBD_TX_OUTCOME
    } else if (cmd == KBD_UART_CMD_TX_OUTCOME) {
        return checksum(rx_buf, 1) == rx_buf[1];
#endif
#if KBD_CRYPT_BENCH_KEY
    } else if (cmd == KBD_UART_CMD_SET_LINK_KEY) {
        return checksum(rx_buf, 17) == rx_buf[17];
    } else if (cmd == KBD_UART_CMD_CRYPT_DIAG) {
        return checksum(rx_buf, 1) == rx_buf[1];
    } else if (cmd == KBD_UART_CMD_CRYPT_FAIL) {
        return checksum(rx_buf, 1) == rx_buf[1];
    } else if (cmd == KBD_UART_CMD_CRYPT_VERIFY) {
        return checksum(rx_buf, 2) == rx_buf[2];
#endif
    }
    return 0;
}

static void dispatch_frame(void)
{
    uint8_t cmd = rx_buf[0];

    if (cmd == 0xA6) {
        KeyboardUart_SendAck();
        if (frame_cb) {
            frame_cb(0xA6, rx_buf[1], 0, 0);
        }
    } else if (cmd == 0xA1) {
        KeyboardUart_SendAck();
        if (frame_cb) {
            frame_cb(0xA1, 0, &rx_buf[1], 8);
        }
    } else if (cmd == 0xA9) {
        uint8_t name_len = rx_buf[1];
        KeyboardUart_SendAck();
        if (frame_cb) {
            frame_cb(0xA9, name_len, &rx_buf[2], name_len);
        }
#if KBD_KEX
    } else if (cmd == KEX_SEED_CMD) {
        KeyboardUart_SendAck();
        if (frame_cb) {
            frame_cb(KEX_SEED_CMD, 0, &rx_buf[2], KEX_SEED_BYTES);
        }
        /* The parser keeps the frame until the next one overwrites it. */
        for (volatile uint8_t *p = &rx_buf[2]; p < &rx_buf[KEX_SEED_PACKET]; p++) {
            *p = 0;
        }
#endif
#if KBD_CRYPT_BENCH_KEY
    } else if (cmd == KBD_UART_CMD_SET_LINK_KEY) {
        KeyboardUart_SendAck();
        if (frame_cb) {
            frame_cb(KBD_UART_CMD_SET_LINK_KEY, 0, &rx_buf[1], 16);
        }
    } else if (cmd == KBD_UART_CMD_CRYPT_DIAG) {
        /* No ack: the 0x5D reply IS the response, and an ack ahead of it just
         * gives the host another frame to skip past. */
        if (frame_cb) {
            frame_cb(KBD_UART_CMD_CRYPT_DIAG, 0, 0, 0);
        }
    } else if (cmd == KBD_UART_CMD_CRYPT_FAIL) {
        /* Same no-ack contract: the 0x5F reply IS the response. */
        if (frame_cb) {
            frame_cb(KBD_UART_CMD_CRYPT_FAIL, 0, 0, 0);
        }
    } else if (cmd == KBD_UART_CMD_CRYPT_VERIFY) {
        KeyboardUart_SendAck();
        if (frame_cb) {
            frame_cb(KBD_UART_CMD_CRYPT_VERIFY, rx_buf[1], 0, 0);
        }
#endif
#if KBD_TX_OUTCOME
    } else if (cmd == KBD_UART_CMD_TX_OUTCOME) {
        /* Same no-ack contract as 0xAF: the 0x62 reply IS the response. */
        if (frame_cb) {
            frame_cb(KBD_UART_CMD_TX_OUTCOME, 0, 0, 0);
        }
#endif
#if KBD_BLE
    } else if (cmd == 0xA7 || cmd == 0xA4) {
        KeyboardUart_SendAck();
        if (frame_cb) {
            frame_cb(cmd, rx_buf[1], 0, 0);
        }
#endif
    } else if (cmd == 0xA3) {
        KeyboardUart_SendAck();
        if (frame_cb) {
            frame_cb(0xA3, 0, &rx_buf[1], 2);
        }
    } else if (cmd == 0xA8) {
        KeyboardUart_SendAck();
        if (frame_cb) {
            /* The sequence byte goes up as `sub`: movement is relative, so
             * the handler must be able to tell a retry of a frame whose ACK
             * was lost from a new one. */
            frame_cb(0xA8, rx_buf[1], &rx_buf[2], 5);
        }
    } else {
        /* Host ACK to one of our 0x5A/0x5B/0x5C frames. */
#if KBD_HOST_WAKE
        HostWake_OnAck(&host_wake);
#endif
    }
}

static void shift_rx_buf(uint8_t n)
{
    if (n >= rx_len) {
        reset_parser();
        return;
    }
    rx_len = (uint8_t)(rx_len - n);
    for (uint8_t i = 0; i < rx_len; i++) {
        rx_buf[i] = rx_buf[i + n];
    }
    rx_expected = expected_for_header(rx_buf[0]);
    if (rx_expected == 0) {
        reset_parser();
    }
}

static void resync_parser(void)
{
    for (uint8_t i = 1; i < rx_len; i++) {
        if (expected_for_header(rx_buf[i])) {
            KBD_UART_DIAG_INC(kbd_uart_rx_resync_count);
            shift_rx_buf(i);
            return;
        }
    }
    reset_parser();
}

static void process_rx_buf(void)
{
    while (rx_len && rx_expected && rx_len >= rx_expected) {
        if (frame_is_valid()) {
            uint8_t consumed = rx_expected;
            dispatch_frame();
            shift_rx_buf(consumed);
        } else {
            KBD_UART_DIAG_INC(kbd_uart_rx_frame_error_count);
#if KBD_KEX
            if (rx_buf[0] == KEX_SEED_CMD) {
                /* A complete seed frame that fails its checksum is dropped
                 * whole. Its body is 32 arbitrary bytes: resynchronising
                 * inside it would let a damaged seed that happens to contain
                 * a well-formed frame (A6 52 F8 is unpair) run as a command.
                 * The host resends the seed when no ACK comes. */
                wipe_rx_buf();
                reset_parser();
                return;
            }
#endif
            resync_parser();
        }
    }
}

static void feed_byte(uint8_t b)
{
    if (rx_len == 0) {
        rx_expected = expected_for_header(b);
        if (rx_expected == 0) {
            return;
        }
    }

    if (rx_len >= KBD_UART_MAX_FRAME) {
        reset_parser();
        return;
    }

    rx_buf[rx_len++] = b;
    /* A 9C whose second byte is not the seed length is NOT given up early:
     * it may be a seed with a damaged length byte, and looking for headers
     * in what follows would run a command that happens to sit in its body.
     * The candidate is collected to 35 bytes and dropped whole, or abandoned
     * by the partial-frame timeout. A stray 9C therefore costs the host the
     * frames it sends in that window; they are retried. */
    process_rx_buf();
}

#if KBD_IDLE_WFI

/* Enabled causes are RECV_RDY and LINE_STAT (see KeyboardUart_Init); with the
 * 1-byte trigger, RECV_RDY fires per byte so a receive-timeout interrupt would
 * be redundant and is not enabled. This handler drains RBR unconditionally
 * because RECV_RDY clears only by reading data, and after a LINE_STAT overrun
 * the FIFO still holds bytes worth keeping. __HIGH_CODE: wake ISRs must run
 * from RAM (the idle path powers flash down; the fetch stall on the way back
 * out is paid once, after we return). */
__INTERRUPT
__HIGH_CODE
void UART1_IRQHandler(void)
{
#if KBD_UART_FAST
    /* A fast build also treats a framing error or a break as a hole in the
     * stream: a byte that arrived damaged, at a rate where one is likelier.
     * (It is not what stops a host at 115200 from being understood; see
     * keyboard_uart.h.) The error belongs to the byte at the head of the
     * FIFO, so LSR is read before each byte is taken; the read clears the
     * overrun bit too, which is kept. The latch is set before the byte is
     * queued and in the same interrupt, which is what lets Poll take a byte
     * and know it came before any hole. */
    if (UART1_GetITFlag() == UART_II_LINE_STAT) {
        if (R8_UART1_LSR & KBD_UART_LSR_RX_ERR) {
            rx_overrun_latch = 1;
        }
    }
    while (R8_UART1_RFC) {
        if (R8_UART1_LSR & KBD_UART_LSR_RX_ERR) {
            rx_overrun_latch = 1;
        }
        uint8_t b = R8_UART1_RBR;
#else
    if (UART1_GetITFlag() == UART_II_LINE_STAT) {
        if (R8_UART1_LSR & RB_LSR_OVER_ERR) {   /* reading LSR clears it */
            rx_overrun_latch = 1;
        }
    }
    while (R8_UART1_RFC) {
        uint8_t b = R8_UART1_RBR;
#endif
        RX_NOTE_ACTIVITY();
        if ((uint8_t)(rx_ring_head - rx_ring_tail) < KBD_UART_RING_SIZE) {
            rx_ring[rx_ring_head & (KBD_UART_RING_SIZE - 1u)] = b;
            rx_ring_head++;
        } else {
            rx_overrun_latch = 1;
        }
    }
}

void KeyboardUart_Poll(void)
{
    uint8_t fed = 0;

    for (;;) {
        /* Checked before EVERY byte, not just once per Poll: the ISR can
         * record a stream hole (hardware overrun or full ring) while this
         * loop is draining, and feeding even one post-hole byte lets a
         * retained pre-gap header swallow the next valid command (review
         * finding). On a hole: discard everything queued and reset the
         * parser; tail=head must be atomic against the ISR (CSR 0x800
         * global-mask idiom, see rf_task.c). Bytes fed before the latch
         * was set are pre-hole and were valid to parse. */
#if KBD_UART_FAST
        /* A fast build closes the gap between that check and the byte it
         * guards: the latch is read and the byte taken under one mask, so a
         * byte that reaches the parser was queued while the latch was still
         * clear. (Unmasked, the ISR could set the latch and queue the
         * damaged byte between the two, and the byte would be parsed before
         * the latch was looked at again.) The byte is parsed with interrupts
         * back on. */
        uint8_t b = 0;
        uint8_t got = 0;
        uint8_t hole;
        uint32_t take_state;

        __asm volatile ("csrrc %0, 0x800, %1"
                        : "=r"(take_state) : "r"(0x88) : "memory");
        hole = rx_overrun_latch;
        if (!hole && rx_ring_tail != rx_ring_head) {
            b = rx_ring[rx_ring_tail & (KBD_UART_RING_SIZE - 1u)];
            /* Cleared before the slot is released: see the 115200 path. */
            rx_ring[rx_ring_tail & (KBD_UART_RING_SIZE - 1u)] = 0;
            rx_ring_tail++;
            got = 1;
        }
        __asm volatile ("csrrs zero, 0x800, %0"
                        :: "r"(take_state & 0x88) : "memory");
        if (hole) {
#else
        if (rx_overrun_latch) {
#endif
            uint32_t irq_state;
            /* What is discarded may be part of an entropy seed: clear it
             * before the slots are released. The bulk goes with interrupts
             * on (the ISR cannot write an occupied slot); the masked section
             * clears only what arrived meanwhile. */
            uint8_t t = rx_ring_tail;
            uint8_t end = rx_ring_head;
            for (; t != end; t++) {
                rx_ring[t & (KBD_UART_RING_SIZE - 1u)] = 0;
            }
            __asm volatile ("csrrc %0, 0x800, %1"
                            : "=r"(irq_state) : "r"(0x88) : "memory");
            for (; t != rx_ring_head; t++) {
                rx_ring[t & (KBD_UART_RING_SIZE - 1u)] = 0;
            }
            rx_ring_tail = rx_ring_head;
            rx_overrun_latch = 0;
            __asm volatile ("csrrs zero, 0x800, %0"
                            :: "r"(irq_state & 0x88) : "memory");
            KBD_UART_DIAG_INC(kbd_uart_rx_overrun_count);
            wipe_rx_buf();
            reset_parser();
#if KBD_UART_FAST
            rx_resync = 1;      /* see KBD_UART_RX_RESYNC_US */
            rx_resync_at = RtcClock_Now();
#endif
            continue;   /* the ISR may already have queued post-gap bytes */
        }
#if KBD_UART_FAST
        if (!got) {
            break;
        }
        if (rx_resync) {
            /* Still inside what the hole was in, or what followed it without
             * a pause: not parsed. (b is a plain local; it is overwritten by
             * the next byte or dies with this call.) */
            rx_resync_at = RtcClock_Now();
            continue;
        }
        feed_byte(b);
#else
        if (rx_ring_tail == rx_ring_head) {
            break;
        }
        feed_byte(rx_ring[rx_ring_tail & (KBD_UART_RING_SIZE - 1u)]);
        /* Cleared before the slot is released (the ISR never writes the tail
         * slot): the ring would otherwise keep a copy of an entropy seed. */
        rx_ring[rx_ring_tail & (KBD_UART_RING_SIZE - 1u)] = 0;
        rx_ring_tail++;
#endif
        fed = 1;
    }
#if KBD_UART_FAST
    /* Every byte taken above restarted the clock, so this much time without
     * one is this much silence on the line, provided nothing has arrived
     * since the loop last looked: interrupts can hold this spot for longer
     * than the wait while the rest of a damaged frame is queued. The time is
     * read first and the ring, the FIFO and the latch are looked at after
     * it, masked, so anything that arrived before that time is seen. If
     * something has, the next Poll discards it and the wait starts again. */
    if (rx_resync) {
        uint32_t now = RtcClock_Now();

        if ((uint32_t)(now - rx_resync_at)
                >= RTC_CLOCK_TICKS_US(KBD_UART_RX_RESYNC_US)) {
            uint32_t quiet_state;

            __asm volatile ("csrrc %0, 0x800, %1"
                            : "=r"(quiet_state) : "r"(0x88) : "memory");
            if (rx_ring_tail == rx_ring_head && R8_UART1_RFC == 0
                    && !rx_overrun_latch) {
                rx_resync = 0;
            }
            __asm volatile ("csrrs zero, 0x800, %0"
                            :: "r"(quiet_state & 0x88) : "memory");
        }
    }
#endif
    expire_partial_frame(fed);
}

#else /* !KBD_IDLE_WFI: original pure-polling path */

static void check_uart_line_status(void)
{
    if (R8_UART1_LSR & RB_LSR_OVER_ERR) {
        KBD_UART_DIAG_INC(kbd_uart_rx_overrun_count);
        wipe_rx_buf();
        reset_parser();
    }
}

void KeyboardUart_Poll(void)
{
    uint8_t fed = 0;

    check_uart_line_status();
    while (R8_UART1_RFC) {
        RX_NOTE_ACTIVITY();
        feed_byte(R8_UART1_RBR);
        check_uart_line_status();
        fed = 1;
    }
    expire_partial_frame(fed);
}

#endif /* KBD_IDLE_WFI */

uint8_t KeyboardUart_RxQuiet(void)
{
#if KBD_IDLE_WFI
    return (rx_ring_tail == rx_ring_head) && (rx_len == 0)
        && !rx_overrun_latch
#if KBD_UART_FAST
        /* Waiting for silence is receive work in progress: Poll has to keep
         * running to end it, so the module neither idles nor sleeps on it. */
        && !rx_resync
#endif
        ;
#else
    return (rx_len == 0);
#endif
}

#if KBD_DEEP_SLEEP
uint8_t KeyboardUart_TakeRxActivity(void)
{
    /* Read-then-clear only when set: if the ISR sets the latch between the
     * read and the clear we still report activity (v == 1), and the new byte
     * sits in the ring where RxQuiet() vetoes sleep -- nothing is lost. */
    uint8_t v = rx_activity_latch;
    if (v) {
        rx_activity_latch = 0;
    }
    return v;
}
#endif

uint8_t KeyboardUart_FifoIdle(void)
{
#if KBD_UART_FAST
    /* This read of LSR clears the receive-error bits like any other, and it
     * can get to them before the interrupt does: the ISR would then queue
     * the damaged byte with nothing to show for it. Kept here instead. Main
     * loop context, as Poll is, so the latch is set before Poll next looks,
     * whichever side of this the interrupt lands. */
    uint8_t lsr;

    if (R8_UART1_TFC != 0) {
        return 0;
    }
    lsr = R8_UART1_LSR;
    if (lsr & KBD_UART_LSR_RX_ERR) {
        rx_overrun_latch = 1;
    }
    return (lsr & RB_LSR_TX_ALL_EMP) != 0;
#else
    return (R8_UART1_TFC == 0) && (R8_UART1_LSR & RB_LSR_TX_ALL_EMP);
#endif
}

uint8_t KeyboardUart_TxIdle(void)
{
    /* Host-wake frames still queued or awaiting their ACK count as TX not
     * idle, so every path that drains TX before sleeping or entering the
     * bootloader also waits for them (or gives up on its own bound). */
    return KeyboardUart_FifoIdle() && KeyboardUart_HostWakeIdle();
}
