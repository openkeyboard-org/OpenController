/*
 * OpenKeyboard.org OpenController
 * Copyright 2026 Eric Molitor (EMulator)
 * hal_aes backed by the portable software cipher instead of the CH592 engine.
 *
 * WHY THIS EXISTS. The CH592 AES accelerator silently returns the PREVIOUS
 * block's output when a block operation is preempted by the BLEB radio
 * interrupt (firmware/docs/TODO.md section 0). Everything defending against
 * that -- kbd_crypt_seal_begin's double-compute, the bounded retry, the engine
 * claim -- exists solely because the accelerator cannot be trusted mid-radio.
 * Measured on this bench, 39-44% of seals hit the collision and are recomputed,
 * which roughly doubles a seal's duration and supplies most of its variance;
 * that variance is what leaves a response slot with no frame ready.
 *
 * Software AES has no engine state to corrupt, so it needs none of that. The
 * per-block cost is higher (~28 us SRAM-resident at 60 MHz against ~14.5 us for
 * the accelerator) but a seal drops from 22-plus blocks of variable cost to 11
 * of fixed cost, and the whole collision class disappears.
 *
 * The key schedule deliberately stays in FLASH: it runs once per key, so making
 * it fast buys nothing, and keeping it out of SRAM is what lets the cipher fit.
 * Only aes_sw_encrypt_block() and the data it touches are charged against the
 * SRAM budget -- same split the CH570 dongle backend uses.
 */
#include "CONFIG.h"        /* __HIGH_CODE */
#include "hal_aes.h"
#include "aes_sw.h"

static aes_sw_ctx_t aes_ctx;

void hal_aes_init(void)
{
    /* Nothing to bring up: there is no peripheral. */
}

void hal_aes_set_key(const uint8_t key[HAL_AES_KEY_BYTES])
{
    aes_sw_expand_key(&aes_ctx, key);
}

/* Cannot fail: no engine to wedge, so no caller ever sees a timeout and the
 * FAULT_ENGINE paths above this seam become unreachable with this backend. */
__HIGH_CODE
hal_aes_status_t hal_aes_encrypt_block(const uint8_t in[HAL_AES_BLOCK_BYTES],
                                       uint8_t out[HAL_AES_BLOCK_BYTES])
{
    aes_sw_encrypt_block(&aes_ctx, in, out);
    return HAL_AES_OK;
}
