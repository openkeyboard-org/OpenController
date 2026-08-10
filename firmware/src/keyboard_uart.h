/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 */
#ifndef KEYBOARD_UART_H
#define KEYBOARD_UART_H

#include <stdint.h>

typedef void (*keyboard_uart_frame_cb_t)(uint8_t cmd, uint8_t sub,
                                         const uint8_t *payload, uint8_t len);

void KeyboardUart_Init(void);
void KeyboardUart_SetFrameCallback(keyboard_uart_frame_cb_t cb);
void KeyboardUart_Poll(void);

void KeyboardUart_SendAck(void);
void KeyboardUart_SendStatus(uint8_t sub);
void KeyboardUart_SendBattery(uint8_t percent);
void KeyboardUart_SendLed(uint8_t led_mask);

/* Nonzero when the TX FIFO is empty AND the transmitter shift register has
 * drained - i.e. every queued byte has physically left the wire. */
uint8_t KeyboardUart_TxIdle(void);

#if KBD_CRYPT_BENCH_KEY
/* BENCH ONLY -- [0xAE][16-byte link key][checksum].
 *
 * Provisions the link key both ends must share until the key-establishment
 * handshake exists. Compiled ONLY under KBD_CRYPT_BENCH_KEY, which is separate
 * from KBD_RF_CRYPT precisely so an encrypted SHIPPING image has no key-write
 * command at all: a link key that any host on the wire could overwrite would
 * hand an attacker the ability to forge keystrokes, which is the exact thing
 * link encryption exists to prevent. Never enable this in a release build. */
#define KBD_UART_CMD_SET_LINK_KEY 0xAEu
#endif

#endif
