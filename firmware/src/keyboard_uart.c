/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 */
#include "CONFIG.h"
#include "HAL.h"
#include "keyboard_uart.h"
#include "rtc_clock.h"

#define KBD_UART_MAX_FRAME  24

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
#define KBD_UART_RING_SIZE 64u   /* power of two, > 2 max frames */
static volatile uint8_t rx_ring[KBD_UART_RING_SIZE];
static volatile uint8_t rx_ring_head;
static volatile uint8_t rx_ring_tail;
/* Set by the ISR on a hardware overrun OR a full ring (same recovery: the
 * byte stream has a hole, so the parser must resync). Consumed by Poll. */
static volatile uint8_t rx_overrun_latch;
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
    UART1_BaudRateCfg(115200);
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

static void reset_parser(void)
{
    rx_len = 0;
    rx_expected = 0;
}

static uint8_t expected_for_header(uint8_t b)
{
    switch (b) {
    case 0xA6:
    case 0x61:
        return 3;
    case 0xA1:
        return 10;
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
    } else if (cmd == 0xA1) {
        return checksum(rx_buf, 9) == rx_buf[9];
    } else if (cmd == 0xA9) {
        uint8_t name_len = rx_buf[1];
        uint8_t chk_idx = (uint8_t)(2 + name_len);
        return (name_len <= 18 && chk_idx < 21 &&
                checksum(rx_buf, chk_idx) == rx_buf[chk_idx]);
    } else if (cmd == 0x61) {
        return rx_buf[1] == 0x0D && rx_buf[2] == 0x0A;
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
    if (UART1_GetITFlag() == UART_II_LINE_STAT) {
        if (R8_UART1_LSR & RB_LSR_OVER_ERR) {   /* reading LSR clears it */
            rx_overrun_latch = 1;
        }
    }
    while (R8_UART1_RFC) {
        uint8_t b = R8_UART1_RBR;
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
    for (;;) {
        /* Checked before EVERY byte, not just once per Poll: the ISR can
         * record a stream hole (hardware overrun or full ring) while this
         * loop is draining, and feeding even one post-hole byte lets a
         * retained pre-gap header swallow the next valid command (review
         * finding). On a hole: discard everything queued and resync from
         * silence; tail=head must be atomic against the ISR (CSR 0x800
         * global-mask idiom, see rf_task.c). Bytes fed before the latch
         * was set are pre-hole and were valid to parse. */
        if (rx_overrun_latch) {
            uint32_t irq_state;
            __asm volatile ("csrrc %0, 0x800, %1"
                            : "=r"(irq_state) : "r"(0x88) : "memory");
            rx_ring_tail = rx_ring_head;
            rx_overrun_latch = 0;
            __asm volatile ("csrrs zero, 0x800, %0"
                            :: "r"(irq_state & 0x88) : "memory");
            KBD_UART_DIAG_INC(kbd_uart_rx_overrun_count);
            reset_parser();
            continue;   /* the ISR may already have queued post-gap bytes */
        }
        if (rx_ring_tail == rx_ring_head) {
            break;
        }
        feed_byte(rx_ring[rx_ring_tail & (KBD_UART_RING_SIZE - 1u)]);
        rx_ring_tail++;
    }
}

#else /* !KBD_IDLE_WFI: original pure-polling path */

static void check_uart_line_status(void)
{
    if (R8_UART1_LSR & RB_LSR_OVER_ERR) {
        KBD_UART_DIAG_INC(kbd_uart_rx_overrun_count);
        reset_parser();
    }
}

void KeyboardUart_Poll(void)
{
    check_uart_line_status();
    while (R8_UART1_RFC) {
        RX_NOTE_ACTIVITY();
        feed_byte(R8_UART1_RBR);
        check_uart_line_status();
    }
}

#endif /* KBD_IDLE_WFI */

uint8_t KeyboardUart_RxQuiet(void)
{
#if KBD_IDLE_WFI
    return (rx_ring_tail == rx_ring_head) && (rx_len == 0)
        && !rx_overrun_latch;
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
    return (R8_UART1_TFC == 0) && (R8_UART1_LSR & RB_LSR_TX_ALL_EMP);
}

uint8_t KeyboardUart_TxIdle(void)
{
    /* Host-wake frames still queued or awaiting their ACK count as TX not
     * idle, so every path that drains TX before sleeping or entering the
     * bootloader also waits for them (or gives up on its own bound). */
    return KeyboardUart_FifoIdle() && KeyboardUart_HostWakeIdle();
}
