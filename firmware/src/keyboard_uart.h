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
 * Reply: [0x5D][n x u32 LE][checksum] -- VARIABLE length, not a fixed 20 bytes.
 * main.c currently sends n = 9, i.e. a 36-byte payload:
 *   seal_miss, tx_sealed, sess_bad, sess_ok, sess_rx,        (the original 5)
 *   selfck_ok, selfck_bad, bb_during_aes, seal_redo.         (bench self-verify)
 * The bench counters are appended AFTER the original five precisely so an
 * older reader still parses the prefix; decode by the frame's own length
 * rather than assuming a count. Keep this list in step with the `counters[]`
 * array in main.c. Read-only, and gated with the bench key command so a
 * shipping build has neither. */
#define KBD_UART_CMD_CRYPT_DIAG 0xAFu
/* [B0][chk] -> [0x5F][latched][len][session:u32 LE][seal_bb][frame:22]
 *              [good_tag:8][plain:8][s1:8][s0:8][chk]:
 * the first self-verify-failed sealed frame this boot, with the recomputed
 * (correct) tag and the count of RF callbacks that landed inside its seal.
 * The trailing plain/s1/s0 vectors are the double-compute evidence and were
 * missing from this comment (KeyboardUart_SendCryptFail takes all four). */
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

#if KBD_TX_OUTCOME
/* BENCH ONLY (KBD_TX_OUTCOME): read the transmit-outcome counters.
 *
 *   request [0xB2][chk]
 *   reply   [0x62][64 x u32 LE][chk]      258 bytes, chk seeded with 0x62
 *
 * Reply order, all u32 LE, every 4-wide group indexed by len_class
 * (0 = 1-byte bare ack, 1 = 10-byte plaintext report, 2 = 22-byte sealed
 * frame, 3 = other):
 *
 *   [ 0.. 3] txo_start    RF_Tx accepted the frame
 *   [ 4.. 7] txo_refuse   RF_Tx refused synchronously
 *   [ 8..11] txo_finish   TX_MODE_TX_FINISH consumed it
 *   [12..15] txo_fail     TX_MODE_TX_FAIL consumed it
 *   [16..19] txo_noterm   superseded with no terminal callback at all
 *   [20..23] txo_other    terminated via the catch-all sta branch
 *   [56..63] txo_arm      why rf_crypt_arm() left no frame, indexed by
 *                         kbd_crypt_status_t (0 OK, 1 SHAPE, 2 INACTIVE,
 *                         3 BUSY, 4 EXHAUSTED, 5 FAULT_ENGINE)
 *   [24..55] txo_dpoll    poll-arrival -> RF_Tx latency, 8 buckets per class,
 *                         136.5 us each (bucket 7 saturates). The connection
 *                         interval is 875 us, so bucket 6+ means the frame
 *                         went out having already missed most of its slot.
 *
 * Class 2 against class 0 is the built-in control: bare acks and sealed frames
 * leave the same slot through the same code, so any difference between them
 * cannot be blamed on the radio or the host.
 *
 * COST: 258 bytes busy-waited through the UART FIFO from the same cooperative
 * main loop that services the poll response -- roughly 18 ms at 115200 baud.
 * Read it BETWEEN trials, never during one, or the reply perturbs the very
 * timing being measured. */
#define KBD_UART_CMD_TX_OUTCOME 0xB2u

/* Emit [0x62][n x u32 LE][checksum]. See KBD_UART_CMD_TX_OUTCOME below.
 * Gated on KBD_TX_OUTCOME alone, NOT on KBD_CRYPT_BENCH_KEY: the whole point is
 * to build a PLAINTEXT control with the same instrument, and a plaintext build
 * has no bench key surface at all. */
void KeyboardUart_SendTxOutcome(const uint32_t *counters, uint8_t n);
#endif
#endif
