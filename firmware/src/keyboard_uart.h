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
/* Emit the self-verify failure latch. See KBD_UART_CMD_CRYPT_FAIL.
 * Reply: [0x5F][latched][len][session u32][seal_bb][frame 22][good 8]
 *        [plain 8][s1 8][s0 8][chk] = 63 bytes. */
void KeyboardUart_SendCryptFail(uint8_t latched, uint8_t len, uint32_t session,
                                uint8_t seal_bb, const uint8_t *frame22,
                                const uint8_t *good8, const uint8_t *plain8,
                                const uint8_t *s1_8, const uint8_t *s0_8);
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
/* [B0][chk] -> [0x5F][latched][len][session:u32 LE][seal_bb][frame:22][good_tag:8][chk]:
 * the first self-verify-failed sealed frame this boot, with the recomputed
 * (correct) tag and the count of RF callbacks that landed inside its seal. */
#define KBD_UART_CMD_CRYPT_FAIL 0xB0u
/* [B1][mode][chk] -> ack: runtime toggle of the bench pre-seal self-verify
 * (mode 0 = off, 1 = on). The verify adds ~73 us of AES immediately before
 * each seal_begin, which SHIFTS THE SEAL'S PHASE in the poll cycle -- and the
 * ~12% idle MAC-failure defect vanished the moment this instrumentation was
 * flashed. Toggling it mid-session, with no reboot and no re-key, is the
 * in-situ A/B that separates "the verify's timing shift masks the defect"
 * from "something else changed". */
#define KBD_UART_CMD_CRYPT_VERIFY 0xB1u
#endif

#endif
