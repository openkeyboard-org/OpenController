/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 *
 * AES-128 block-cipher seam.
 *
 * PROVENANCE: ported from OpenDongle (Apache-2.0, same authorship) at
 * `firmware/common/include/hal_aes.h`, branch `em-rf-ccm-rx-decrypt`. The
 * contract below is reproduced because callers depend on every clause of it;
 * the CH570 software-backend timing tables are dropped -- this firmware is
 * CH592-only and has one backend. Consult the OpenDongle copy for those.
 *
 * The receiver decrypts what this seam encrypts, so both ends MUST agree
 * bit-for-bit. They do: CCM derives both its CBC-MAC and its CTR keystream by
 * ENCRYPTING, and ciphertext is ciphertext regardless of which backend
 * produced a block.
 *
 * CONTRACT (an implementation MUST honor all of it):
 *
 *  - AES-128 ONLY. 16-byte key, 16-byte block.
 *  - FORWARD CIPHER ONLY. There is deliberately no decrypt entry point. The
 *    construction is counter mode, which derives a keystream by ENCRYPTING a
 *    nonce block and XORing it with the payload -- both directions of a CTR
 *    link use the forward cipher. If a mode that genuinely needs the inverse
 *    cipher is ever adopted, add it deliberately rather than assuming this
 *    seam already provides it.
 *  - KEY IS SCHEDULED SEPARATELY from encryption. Set the key once per key
 *    change, NOT once per block.
 *  - hal_aes_encrypt_block() is PURE with respect to the key: same key, same
 *    input, same output, no cross-block chaining state. The caller builds the
 *    mode.
 *  - IN-PLACE IS ALLOWED and any overlap is safe: all 16 input bytes are read
 *    before any output byte is written.
 *  - COST on CH592 at 60 MHz, measured on silicon: 871 cycles/block = 14.5 us;
 *    key schedule 838 cycles (key in SRAM, which is the production regime --
 *    keys come from the bond record in RAM). Budget against the 100 us
 *    poll-to-response turnaround (RF_TURNAROUND_COUNT, rf_task.c): one block
 *    is ~14.5% of it. That is why the CCM caller precomputes everything it can
 *    outside the turnaround window.
 *  - NOT REENTRANT. A single shared hardware engine plus module state, so a
 *    call from interrupt context that preempts one in task context corrupts
 *    the result. The RF caller must serialize main-loop and ISR-path use.
 *  - TREAT AS NOT CONSTANT-TIME. The engine is opaque; nothing is known about
 *    its internal timing either way. Do NOT reuse this seam anywhere a local
 *    attacker can measure it. The threat model here is an attacker over the
 *    air, who cannot observe instruction timing.
 */
#ifndef HAL_AES_H
#define HAL_AES_H

#include <stdint.h>

#define HAL_AES_BLOCK_BYTES 16u
#define HAL_AES_KEY_BYTES   16u

/* Bring the backend up. Safe to call more than once. Call before
 * hal_aes_set_key(). */
void hal_aes_init(void);

/* Install the 16-byte key. Call once per key, not once per block. */
void hal_aes_set_key(const uint8_t key[HAL_AES_KEY_BYTES]);

typedef enum {
    HAL_AES_OK = 0,             /* `out` holds the ciphertext */
    HAL_AES_ENGINE_TIMEOUT = 1  /* the engine did not complete */
} hal_aes_status_t;

/* Encrypt one 16-byte block under the key from the last hal_aes_set_key().
 * `out` may alias `in`. Behaviour is undefined if no key has been set.
 *
 * CHECK THE RETURN VALUE -- here, ignoring it is worse than a normal unchecked
 * error. If the engine wedges this returns HAL_AES_ENGINE_TIMEOUT and `out` is
 * ZEROED rather than left holding whatever the engine's data registers held.
 *
 * The zeroing is the lesser of two bad outcomes, not a safe one. In counter
 * mode the output is a keystream block, and without the check a wedged engine
 * returns THE PLAINTEXT -- measured, not assumed: the input is written into the
 * data registers before the engine starts, so if it never runs, reading them
 * back yields exactly what was written. The caller would then XOR that
 * "keystream" with the same plaintext and transmit the result, which is not
 * encryption at all. Zeroing instead degrades to transmitting plaintext, which
 * is equally broken but loud -- visible in a capture rather than silently
 * self-cancelling.
 *
 * Neither is acceptable on air. A caller that gets HAL_AES_ENGINE_TIMEOUT MUST
 * NOT transmit the block: treat it as a fatal fault of the radio path, not a
 * retryable condition. */
hal_aes_status_t hal_aes_encrypt_block(const uint8_t in[HAL_AES_BLOCK_BYTES],
                                       uint8_t out[HAL_AES_BLOCK_BYTES]);

#endif /* HAL_AES_H */
