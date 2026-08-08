/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 */
#include "CONFIG.h"
#include "HAL.h"
#include "keyboard_uart.h"

#define KBD_UART_MAX_FRAME  24

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

static keyboard_uart_frame_cb_t frame_cb;
static uint8_t rx_buf[KBD_UART_MAX_FRAME];
static uint8_t rx_len;
static uint8_t rx_expected;

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
    uint32_t start = SYS_GetSysTickCnt();
    uint32_t limit = (GetSysClock() / 1000000u) * KBD_UART_TX_WAIT_US;

    while (R8_UART1_TFC == UART_FIFO_SIZE) {
        if ((uint32_t)(SYS_GetSysTickCnt() - start) >= limit) {
            return 0;   /* host not draining -> abort, don't freeze the loop */
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
    GPIOPinRemap(ENABLE, RB_PIN_UART1);
    GPIOB_SetBits(bTXD1_);
    GPIOB_ModeCfg(bTXD1_, GPIO_ModeOut_PP_5mA);
    GPIOB_ModeCfg(bRXD1_, GPIO_ModeIN_PU);

    UART1_DefInit();
    UART1_BaudRateCfg(115200);
    UART1_CLR_RXFIFO();
    UART1_CLR_TXFIFO();
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

void KeyboardUart_SendStatus(uint8_t sub)
{
    uart_send_frame(0x5B, sub);
}

void KeyboardUart_SendBattery(uint8_t percent)
{
    uart_send_frame(0x5C, percent);
}

void KeyboardUart_SendLed(uint8_t led_mask)
{
    uart_send_frame(0x5A, led_mask);
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
        feed_byte(R8_UART1_RBR);
        check_uart_line_status();
    }
}

uint8_t KeyboardUart_TxIdle(void)
{
    return (R8_UART1_TFC == 0) && (R8_UART1_LSR & RB_LSR_TX_ALL_EMP);
}
