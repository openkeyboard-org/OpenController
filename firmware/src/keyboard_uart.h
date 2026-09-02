/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef KEYBOARD_UART_H
#define KEYBOARD_UART_H

#include <stdint.h>

/* Idle-state WFI (power ladder MR2): interrupt-driven UART RX + the
 * LowPower_Idle() site in Main_Circulation. Default on; disable for a
 * pure-polling A/B baseline with EXTRA_CFLAGS=-DKBD_IDLE_WFI=0. */
#ifndef KBD_IDLE_WFI
#define KBD_IDLE_WFI 1
#endif

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

/* Nonzero when no received byte is waiting anywhere on the RX path: ring
 * buffer empty (KBD_IDLE_WFI builds), parser between frames, no latched
 * line error. Hardware FIFO state is deliberately NOT included - the WFI
 * site re-checks R8_UART1_RFC itself under masked IRQs. */
uint8_t KeyboardUart_RxQuiet(void);

#endif
