/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
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

#endif
