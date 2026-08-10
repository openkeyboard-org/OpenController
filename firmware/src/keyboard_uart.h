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

#if KBD_CRYPT_BENCH_KEY
/* Emit [0x5D][n x u32 LE][checksum]. See KBD_UART_CMD_CRYPT_DIAG. */
void KeyboardUart_SendCryptDiag(const uint32_t *counters, uint8_t n);
#endif

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

/* Bench: dump the link-encryption counters over UART.
 *
 * These live in .diag_safe and were previously read over SWD, which is wrong
 * twice on this part: firmware/README.md documents that minichlink's CH5xx
 * memory reads are unreliable (a known-good SRAM address can read back all
 * zeros), and that ANY debug attach to a running application effectively resets
 * the part. Both corrupted this investigation -- an all-zero read was taken as
 * evidence of a reboot, and the reset-per-attach was measured as a phantom
 * periodic crash. The README's own advice is to instrument the firmware and
 * read it out of band; this is that channel for the keyboard, matching what
 * CMD_CRYPT_DIAG already does for the receiver.
 *
 * Reply: [0x5D][20 bytes: seal_miss, tx_sealed, sess_bad, sess_ok, sess_rx,
 * each u32 LE][checksum]. Read-only, and gated with the bench key command so a
 * shipping build has neither. */
#define KBD_UART_CMD_CRYPT_DIAG 0xAFu
#endif

#endif
