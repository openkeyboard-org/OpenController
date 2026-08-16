/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 *
 * AES-128-CCM link encryption -- see kbd_rf_crypt.h for the wire format, the
 * counter discipline, and the reentrancy contract.
 *
 * The CCM primitives below are a deliberate mirror of OpenDongle's
 * `firmware/common/src/rf_crypt.c` (branch `em-rf-ccm-rx-decrypt`). They have
 * no design freedom: the receiver recomputes exactly these bytes, so any
 * divergence shows up as a link that pairs and then drops every frame on the
 * MAC check. Both sides are graded against OpenDongle's tests/ccm_ref.py.
 */

#include "kbd_rf_crypt.h"

#include "hal_aes.h"

/* ------------------------------------------------------------------ state */

/* Seal double-compute disagreements (each is one radio/AES collision caught
 * before it could poison a frame; the seal was recomputed). Plain .bss. */
uint32_t kbd_crypt_seal_redo;

static uint8_t  crypt_key_ready;
static uint8_t  crypt_session_ready;
static uint32_t crypt_session_id;
static uint32_t crypt_tx_ctr;        /* last counter CONSUMED; next is +1 */

/* Engine ownership. RV32IMAC has the A extension, so the GCC atomic builtins
 * lower to amoswap.w -- a genuine test-and-set, not a read-then-write an
 * interrupt can land inside. */
static volatile uint8_t crypt_engine_busy;

#if KBD_CRYPT_BENCH_KEY
/* Bench self-verify state; see the header block. Plain .bss (zeroed at boot),
 * read over UART only. */
volatile uint8_t kbd_crypt_in_aes;
volatile uint8_t kbd_crypt_selfck_enable = 1u;   /* UART 0xB1 toggles */
uint32_t kbd_crypt_bb_during_aes;
volatile uint8_t kbd_crypt_seal_bb;
uint32_t kbd_crypt_selfck_ok;
uint32_t kbd_crypt_selfck_bad;
uint8_t  kbd_crypt_selfck_latched;
uint8_t  kbd_crypt_selfck_len;
uint32_t kbd_crypt_selfck_session;
uint8_t  kbd_crypt_selfck_frame[KBD_CRYPT_LEN_BOOT_KBD];
uint8_t  kbd_crypt_selfck_good[KBD_CRYPT_TAG_BYTES];
uint8_t  kbd_crypt_selfck_seal_bb;
/* Block-level evidence for the latched failure: the verify's own recovered
 * plaintext and both keystream blocks. An idle keepalive's plain is all-zero,
 * so plain != 0 here convicts the s1 block at the verify call site. */
uint8_t  kbd_crypt_selfck_plain[KBD_CRYPT_MAX_BODY];
uint8_t  kbd_crypt_selfck_s1[8];
uint8_t  kbd_crypt_selfck_s0[8];

static uint8_t  bench_pend_valid;
static uint8_t  bench_pend_len;
static uint8_t  bench_pend_seal_bb;
static uint32_t bench_pend_session;
static uint8_t  bench_pend_frame[KBD_CRYPT_LEN_BOOT_KBD];

#define KBD_BENCH_AES_BEGIN() do { kbd_crypt_seal_bb = 0u; \
                                   kbd_crypt_in_aes = 1u; } while (0)
#define KBD_BENCH_AES_END()   do { kbd_crypt_in_aes = 0u; } while (0)
#else
#define KBD_BENCH_AES_BEGIN() do { } while (0)
#define KBD_BENCH_AES_END()   do { } while (0)
#endif

/* seal_begin() -> seal_finish() carry state. seal_mic holds one finished tag per
 * reachable ctrl value, which is what lets seal_finish() run without touching
 * the AES engine at all -- see the header's note on the response path. */
static uint8_t  seal_pending;
static uint8_t  seal_tag;
static uint8_t  seal_body_len;
static uint32_t seal_ctr;
static uint8_t  seal_ctrl_base;                  /* ctrl bits above the ARQ pair */
static uint8_t  seal_cipher[KBD_CRYPT_MAX_BODY];
static uint8_t  seal_mic[KBD_CRYPT_CTRL_VARIANTS][KBD_CRYPT_TAG_BYTES];

/* ---------------------------------------------------------------- helpers */

static void xor16(uint8_t *acc, const uint8_t *v)
{
    uint8_t i;
    for (i = 0; i < 16u; i++) {
        acc[i] ^= v[i];
    }
}

/* CCM flags byte for the CBC-MAC B0 block: Adata | ((M-2)/2)<<3 | (L-1).
 * M=8, L=2 -> 0x59 with AAD present. */
static uint8_t ccm_b0_flags(uint8_t have_aad)
{
    return (uint8_t)((have_aad ? 0x40u : 0x00u) | (((8u - 2u) / 2u) << 3) | (2u - 1u));
}

static void build_nonce(uint8_t nonce[13], uint32_t session_id,
                        uint8_t direction, uint32_t counter)
{
    nonce[0]  = (uint8_t)session_id;
    nonce[1]  = (uint8_t)(session_id >> 8);
    nonce[2]  = (uint8_t)(session_id >> 16);
    nonce[3]  = (uint8_t)(session_id >> 24);
    nonce[4]  = direction;
    nonce[5]  = (uint8_t)counter;
    nonce[6]  = (uint8_t)(counter >> 8);
    nonce[7]  = (uint8_t)(counter >> 16);
    nonce[8]  = (uint8_t)(counter >> 24);
    nonce[9]  = 0u;
    nonce[10] = 0u;
    nonce[11] = 0u;
    nonce[12] = 0u;
}

/* CTR keystream block A_i for counter index i (L=2). */
static hal_aes_status_t ctr_block(const uint8_t nonce[13], uint16_t i, uint8_t out[16])
{
    uint8_t a[16];
    uint8_t k;
    a[0] = (uint8_t)(2u - 1u);           /* counter-block flags: L'=L-1 */
    for (k = 0; k < 13u; k++) {
        a[1 + k] = nonce[k];
    }
    a[14] = (uint8_t)(i >> 8);
    a[15] = (uint8_t)i;
    return hal_aes_encrypt_block(a, out);
}

/* First CBC-MAC block: B0 = flags || nonce || l(m), big-endian length. */
static hal_aes_status_t cbc_mac_b0(const uint8_t nonce[13], uint8_t aad_len,
                                   uint8_t msg_len, uint8_t x_out[16])
{
    uint8_t blk[16];
    uint8_t i;

    blk[0] = ccm_b0_flags(aad_len != 0u);
    for (i = 0; i < 13u; i++) {
        blk[1 + i] = nonce[i];
    }
    blk[14] = (uint8_t)(msg_len >> 8);
    blk[15] = (uint8_t)msg_len;
    return hal_aes_encrypt_block(blk, x_out);
}

/* AAD block: 2-byte big-endian length prefix, then AAD, zero-padded.
 * aad_len <= 14 so it is always exactly one block. */
static hal_aes_status_t cbc_mac_aad(uint8_t x[16], const uint8_t *aad, uint8_t aad_len)
{
    uint8_t blk[16];
    uint8_t i;

    for (i = 0; i < 16u; i++) {
        blk[i] = 0u;
    }
    blk[0] = (uint8_t)(aad_len >> 8);
    blk[1] = (uint8_t)aad_len;
    for (i = 0; i < aad_len; i++) {
        blk[2 + i] = aad[i];
    }
    xor16(x, blk);
    return hal_aes_encrypt_block(x, x);
}

/* Message block: zero-padded. A zero-length message contributes NO block --
 * load-bearing for the empty-payload session frame. */
static hal_aes_status_t cbc_mac_msg(uint8_t x[16], const uint8_t *msg, uint8_t msg_len)
{
    uint8_t blk[16];
    uint8_t i;

    if (msg_len == 0u) {
        return HAL_AES_OK;
    }
    for (i = 0; i < 16u; i++) {
        blk[i] = 0u;
    }
    for (i = 0; i < msg_len; i++) {
        blk[i] = msg[i];
    }
    xor16(x, blk);
    return hal_aes_encrypt_block(x, x);
}

/* ---------------------------------------------------------------- lifecycle */

void kbd_crypt_init(void)
{
    hal_aes_init();
}

void kbd_crypt_install_key(const uint8_t key[KBD_CRYPT_KEY_BYTES],
                           uint32_t ctr_start)
{
    hal_aes_set_key(key);
    crypt_key_ready = 1u;
    /* A new key voids any session: the old session_id/counter pair says nothing
     * about replay under a different key. */
    crypt_session_ready = 0u;
    crypt_session_id = 0u;
    /* Clamped so a high start cannot strand us near exhaustion -- see the
     * header for why the start varies at all. */
    crypt_tx_ctr = (ctr_start > KBD_CRYPT_CTR_START_MAX)
                 ? (ctr_start & KBD_CRYPT_CTR_START_MAX)
                 : ctr_start;
    seal_pending = 0u;
}

void kbd_crypt_adopt_session(uint32_t session_id)
{
    /* THE COUNTER IS NOT RESET HERE. It is monotonic for the lifetime of the
     * installed key, across every session adopted under it. That is a
     * deliberate departure from the obvious "restart at 1 per session", and it
     * is what keeps the nonce unique.
     *
     * The nonce is session_id || direction || counter. Restarting the counter
     * makes nonce uniqueness depend entirely on session_id never repeating,
     * and it does repeat, in two ways:
     *
     *  1. WITHOUT ANY ATTACKER. The receiver announces one session up to
     *     RF_CRYPT_ANNOUNCE_POLLS (8) times and stops early only once a frame
     *     of ours verifies. Announcements 2..8 can therefore land after we have
     *     already sent frames 1..n under that same session. Restarting would
     *     re-issue those exact nonces with different plaintext -- keystream
     *     reuse, which XORs two ciphertexts into the XOR of two keystrokes.
     *  2. WITH ONE. A session frame is authenticated but carries no freshness,
     *     so a recorded one stays valid forever under the same key. Replaying
     *     an old session frame would drag us back to a previous session_id and
     *     restart the counter into nonces already used under it.
     *
     * A monotonic counter removes both: the (session_id, counter) pair cannot
     * repeat while the counter cannot repeat, whatever happens to session_id.
     * It costs nothing on the wire and needs no receiver change -- the receiver
     * only requires a counter that is non-zero and above its high-water mark,
     * and it resets that mark to 0 when it mints a session, so a counter that
     * simply keeps climbing always satisfies it.
     *
     * What a replayed session frame can still do is desynchronise us onto a
     * session the receiver is not using, so our frames fail its MAC until it
     * re-announces -- a nuisance the silence guard recovers from, not a key
     * compromise. Closing that needs freshness in the session frame, which is a
     * wire-format change, and is noted for the key-establishment phase.
     *
     * Residual: across a keyboard reboot the counter restarts from RAM, so
     * uniqueness falls back to session_id not repeating -- the receiver's own
     * documented 32-bit birthday bound (~1% after ~9,300 sessions under one
     * long-lived key). Per-session keys from the establishment phase void it. */
    crypt_session_id = session_id;
    crypt_session_ready = 1u;
    seal_pending = 0u;         /* any half-built frame belongs to the old session */
}

void kbd_crypt_end_session(void)
{
    /* The counter is NOT reset here. It survives for the lifetime of the
     * installed key, which is the invariant kbd_crypt_adopt_session() documents
     * and depends on. Resetting it here quietly broke that: rf_enter_connected()
     * ends the session on every connect, so the counter restarted per
     * connection, and an attacker replaying an old (freshness-free) session
     * announce after a reconnect could get counter 1 re-issued under a session
     * whose ciphertexts they already hold -- keystream reuse, and the XOR of two
     * keystroke reports falls out. Only installing a key rewinds the counter,
     * because that genuinely is a fresh nonce space.
     *
     * A residual remains across a keyboard REBOOT, where the counter restarts
     * from RAM and uniqueness falls back to the receiver not repeating a 32-bit
     * session id. Closing that needs a persisted counter or per-session keys. */
    crypt_session_ready = 0u;
    crypt_session_id = 0u;
    seal_pending = 0u;
}

void kbd_crypt_clear(void)
{
    static const uint8_t zero_key[KBD_CRYPT_KEY_BYTES] = { 0 };
    uint8_t i;

    hal_aes_set_key(zero_key);   /* zeroize the installed schedule */
    crypt_key_ready = 0u;
    crypt_session_ready = 0u;
    crypt_session_id = 0u;
    crypt_tx_ctr = 0u;
    seal_pending = 0u;
    for (i = 0; i < KBD_CRYPT_MAX_BODY; i++) {
        seal_cipher[i] = 0u;
    }
    for (i = 0; i < KBD_CRYPT_CTRL_VARIANTS; i++) {
        uint8_t j;
        for (j = 0; j < KBD_CRYPT_TAG_BYTES; j++) {
            seal_mic[i][j] = 0u;
        }
    }
}

int kbd_crypt_active(void)
{
    return (crypt_key_ready != 0u) && (crypt_session_ready != 0u);
}

int kbd_crypt_keyed(void)
{
    return crypt_key_ready != 0u;
}

uint32_t kbd_crypt_session_id(void)
{
    return crypt_session_id;
}

/* ------------------------------------------------------ engine serialization */

int kbd_crypt_try_claim(void)
{
    return __atomic_exchange_n(&crypt_engine_busy, 1u, __ATOMIC_ACQUIRE) == 0u;
}

void kbd_crypt_release(void)
{
    __atomic_store_n(&crypt_engine_busy, 0u, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ shapes */

uint8_t kbd_crypt_encrypted_body_len(uint8_t tag, uint8_t len)
{
    if (tag == KBD_CRYPT_TAG_BOOT_KBD && len == KBD_CRYPT_LEN_BOOT_KBD) {
        return 8u;
    }
    if (tag == KBD_CRYPT_TAG_CONSUMER && len == KBD_CRYPT_LEN_CONSUMER) {
        return 2u;
    }
    if (tag == KBD_CRYPT_TAG_MOUSE && len == KBD_CRYPT_LEN_MOUSE) {
        return 5u;
    }
    return 0u;
}

/* Body length this firmware will seal for a given tag, else 0. The inverse of
 * the table above, keyed on the tag alone. */
static uint8_t seal_body_len_for_tag(uint8_t tag)
{
    if (tag == KBD_CRYPT_TAG_BOOT_KBD) {
        return 8u;
    }
    if (tag == KBD_CRYPT_TAG_CONSUMER) {
        return 2u;
    }
    if (tag == KBD_CRYPT_TAG_MOUSE) {
        return 5u;
    }
    return 0u;
}

/* ---------------------------------------------------------------- transmit */

kbd_crypt_status_t kbd_crypt_seal_begin(uint8_t ctrl_hint, uint8_t tag,
                                        const uint8_t *body, uint8_t body_len)
{
    uint8_t nonce[13];
    uint8_t s0[16];
    uint8_t s1[16];
    uint8_t x1[16];
    uint8_t x[16];
    uint8_t aad[2];
    uint8_t plain[KBD_CRYPT_MAX_BODY];
    uint8_t v;
    uint8_t i;

    if (!kbd_crypt_active()) {
        return KBD_CRYPT_INACTIVE;
    }
    if (body == 0 || body_len == 0u || body_len > KBD_CRYPT_MAX_BODY ||
        seal_body_len_for_tag(tag) != body_len) {
        return KBD_CRYPT_SHAPE;
    }
    /* Counter 0 is reserved and the space must never wrap: a repeated counter
     * under one session is nonce reuse. Refuse instead, and let the caller
     * force a re-key. At one frame per 875 us poll this is ~43 days of
     * continuous transmission, so it is a guard, not a scheduled event. */
    if (crypt_tx_ctr == 0xFFFFFFFFu) {
        return KBD_CRYPT_EXHAUSTED;
    }
    /* Self-enforce the non-reentrancy contract rather than trusting callers. */
    if (!kbd_crypt_try_claim()) {
        return KBD_CRYPT_BUSY;
    }
    KBD_BENCH_AES_BEGIN();   /* bench: count RF callbacks landing in this seal */

    /* Invalidate any frame already waiting BEFORE overwriting the state it is
     * made of. This call replaces a pending seal, and every early return below
     * leaves that state half-rewritten: new counter and ciphertext, MAC
     * variants from the previous frame. Leaving seal_pending set across that
     * would let seal_finish() publish the new ciphertext under a stale tag, and
     * -- because crypt_tx_ctr is only committed on success -- the next seal
     * would hand out the same counter again, reusing the CTR keystream. Clear
     * first, so a failed seal yields no frame at all rather than a poisoned
     * one. Re-issuing the counter after this point is safe precisely because
     * nothing was ever published under it. */
    seal_pending = 0u;

    seal_ctr = crypt_tx_ctr + 1u;
    build_nonce(nonce, crypt_session_id, KBD_CRYPT_DIR_KB_TO_DONGLE, seal_ctr);

    for (i = 0; i < body_len; i++) {
        plain[i] = body[i];
    }

    /* Compute the WHOLE seal twice and require the passes to agree.
     *
     * The engine shares a datapath with the radio, and a BLEB preempt
     * mid-block silently aborts the operation: CFG reads back complete while
     * the DATA latch still holds the PREVIOUS block's output (docs/TODO.md
     * section 0). Arming the seal after TX_FINISH removed most exposure, but
     * the poll grid keeps the radio active every 875 us, so ~0.5% of seals
     * still caught a corrupted block on the bench. An aborted block returns
     * stale bytes that an honest recompute cannot reproduce, so pass
     * disagreement detects it; both passes aborting AT the same block with
     * the same stale latch content is the only blind spot, and the collision
     * rate squared puts that below one seal in ten million. One bounded
     * retry, then KBD_CRYPT_BUSY: no frame this slot (the response path sends
     * its bare ack), and the caller's re-arm tries again in the next cycle --
     * never a poisoned frame, and never a torn-down session for a transient. */
    for (uint8_t attempt = 0; ; attempt++) {
        uint8_t s0b[16];
        uint8_t xb[16];
        uint8_t diff;

        /* Pass 1: S_0 masks the tag; S_1 is the payload keystream (bodies are
         * <= 8 bytes, so exactly one keystream block). B0 depends only on the
         * nonce and the message length, not on ctrl. */
        if (ctr_block(nonce, 0u, s0) != HAL_AES_OK ||
            ctr_block(nonce, 1u, s1) != HAL_AES_OK ||
            cbc_mac_b0(nonce, 2u, body_len, x1) != HAL_AES_OK) {
            KBD_BENCH_AES_END();
            kbd_crypt_release();
            return KBD_CRYPT_FAULT_ENGINE;
        }

        for (i = 0; i < body_len; i++) {
            seal_cipher[i] = (uint8_t)(plain[i] ^ s1[i]);
        }

        /* Finish the MAC for EVERY reachable ctrl value, not just the one we
         * happen to know now. ctrl is AAD and is only latched when the poll
         * arrives, so this is what buys a response path with no AES in it at
         * all. Only the two ARQ bits vary (rf_task.c seeds tx_ctrl and
         * thereafter replaces bit 0 and toggles bit 1, never touching the
         * rest), so four tags cover it. */
        seal_ctrl_base = (uint8_t)(ctrl_hint & (uint8_t)~KBD_CRYPT_CTRL_VARIANT_MASK);
        for (v = 0; v < KBD_CRYPT_CTRL_VARIANTS; v++) {
            for (i = 0; i < 16u; i++) {
                x[i] = x1[i];
            }
            aad[0] = (uint8_t)(seal_ctrl_base | v);
            aad[1] = tag;
            if (cbc_mac_aad(x, aad, 2u) != HAL_AES_OK ||
                cbc_mac_msg(x, plain, body_len) != HAL_AES_OK) {
                KBD_BENCH_AES_END();
                kbd_crypt_release();
                return KBD_CRYPT_FAULT_ENGINE;
            }
            for (i = 0; i < KBD_CRYPT_TAG_BYTES; i++) {
                seal_mic[v][i] = (uint8_t)(x[i] ^ s0[i]);
            }
        }

        /* Pass 2: re-derive every block into scratch and accumulate the
         * differences (no early exit; the comparison is not secret-dependent
         * in a way that matters, but staying branch-free is free here). */
        diff = 0u;
        if (ctr_block(nonce, 0u, s0b) != HAL_AES_OK ||
            ctr_block(nonce, 1u, xb) != HAL_AES_OK) {
            KBD_BENCH_AES_END();
            kbd_crypt_release();
            return KBD_CRYPT_FAULT_ENGINE;
        }
        for (i = 0; i < 16u; i++) {
            diff |= (uint8_t)(s0[i] ^ s0b[i]);
        }
        for (i = 0; i < body_len; i++) {
            diff |= (uint8_t)(seal_cipher[i] ^ (uint8_t)(plain[i] ^ xb[i]));
        }
        if (cbc_mac_b0(nonce, 2u, body_len, xb) != HAL_AES_OK) {
            KBD_BENCH_AES_END();
            kbd_crypt_release();
            return KBD_CRYPT_FAULT_ENGINE;
        }
        for (i = 0; i < 16u; i++) {
            diff |= (uint8_t)(x1[i] ^ xb[i]);
        }
        for (v = 0; v < KBD_CRYPT_CTRL_VARIANTS; v++) {
            for (i = 0; i < 16u; i++) {
                x[i] = x1[i];
            }
            aad[0] = (uint8_t)(seal_ctrl_base | v);
            aad[1] = tag;
            if (cbc_mac_aad(x, aad, 2u) != HAL_AES_OK ||
                cbc_mac_msg(x, plain, body_len) != HAL_AES_OK) {
                KBD_BENCH_AES_END();
                kbd_crypt_release();
                return KBD_CRYPT_FAULT_ENGINE;
            }
            for (i = 0; i < KBD_CRYPT_TAG_BYTES; i++) {
                diff |= (uint8_t)(seal_mic[v][i] ^ (uint8_t)(x[i] ^ s0b[i]));
            }
        }

        if (diff == 0u) {
            break;
        }
        kbd_crypt_seal_redo++;
        if (attempt != 0u) {
            /* Two disagreeing attempts: stand down for this slot rather than
             * risk a poisoned frame. Not FAULT_ENGINE -- the engine answers,
             * the radio is just colliding with it -- so the session survives
             * and the next arm retries. */
            KBD_BENCH_AES_END();
            kbd_crypt_release();
            return KBD_CRYPT_BUSY;
        }
    }

    seal_tag = tag;
    seal_body_len = body_len;
    /* The counter is consumed HERE, not at finish(): a begin() whose finish()
     * never happens must burn its value rather than let a later frame reuse it.
     * Gaps are fine -- the receiver only requires strictly increasing. */
    crypt_tx_ctr = seal_ctr;
    seal_pending = 1u;
    KBD_BENCH_AES_END();
    kbd_crypt_release();
    return KBD_CRYPT_OK;
}

kbd_crypt_status_t kbd_crypt_seal_finish(uint8_t ctrl,
                                         uint8_t *out, uint8_t *out_len)
{
    uint8_t i;
    uint8_t n;
    uint8_t v;

    if (!seal_pending || out == 0 || out_len == 0) {
        return KBD_CRYPT_SHAPE;
    }
    /* The MAC for this ctrl must already exist. It always will -- begin()
     * covered every reachable value -- so a miss means the caller's ctrl left
     * the range the ARQ logic can produce. Refuse rather than reach for the AES
     * engine here: this runs in the response path, where a crypto call would
     * both blow the turnaround budget and race main-loop use of the engine. */
    if ((uint8_t)(ctrl & (uint8_t)~KBD_CRYPT_CTRL_VARIANT_MASK) != seal_ctrl_base) {
        return KBD_CRYPT_SHAPE;
    }
    v = (uint8_t)(ctrl & KBD_CRYPT_CTRL_VARIANT_MASK);

    /* Consume the pending frame: this counter value is spent and the frame must
     * not be emitted twice. */
    seal_pending = 0u;
    n = seal_body_len;

    out[0] = ctrl;
    out[1] = seal_tag;
    out[2] = (uint8_t)seal_ctr;
    out[3] = (uint8_t)(seal_ctr >> 8);
    out[4] = (uint8_t)(seal_ctr >> 16);
    out[5] = (uint8_t)(seal_ctr >> 24);
    for (i = 0; i < n; i++) {
        out[6 + i] = seal_cipher[i];
    }
    for (i = 0; i < KBD_CRYPT_TAG_BYTES; i++) {
        out[6 + n + i] = seal_mic[v][i];
    }
    *out_len = (uint8_t)(n + KBD_CRYPT_FRAME_OVERHEAD);
    return KBD_CRYPT_OK;
}

int kbd_crypt_seal_pending(void)
{
    return seal_pending != 0u;
}

void kbd_crypt_seal_discard(void)
{
    /* Cheap and cipher-free, which is the point: a caller that learns the
     * prepared frame is stale (its report has been superseded) can invalidate it
     * without doing 11 AES blocks wherever it happens to be running. The
     * replacement seal is then built in a slot chosen for it. */
    seal_pending = 0u;
}

kbd_crypt_status_t kbd_crypt_seal(uint8_t ctrl, uint8_t tag,
                                  const uint8_t *body, uint8_t body_len,
                                  uint8_t *out, uint8_t *out_len)
{
    kbd_crypt_status_t st = kbd_crypt_seal_begin(ctrl, tag, body, body_len);
    if (st != KBD_CRYPT_OK) {
        return st;
    }
    return kbd_crypt_seal_finish(ctrl, out, out_len);
}

/* ----------------------------------------------------------------- receive */

kbd_crypt_status_t kbd_crypt_verify_session(const uint8_t *frame, uint8_t len,
                                            uint32_t *out_session_id)
{
    uint8_t nonce[13];
    uint8_t aad[2];
    uint8_t x[16];
    uint8_t s0[16];
    uint32_t sid;
    uint8_t diff = 0u;
    uint8_t i;

    if (frame == 0 || out_session_id == 0) {
        return KBD_CRYPT_SHAPE;
    }
    if (len != KBD_CRYPT_LEN_SESSION || frame[1] != KBD_CRYPT_TAG_SESSION) {
        return KBD_CRYPT_SHAPE;
    }
    /* Only the key is required. The session being authenticated is the one
     * carried in this frame, so there is nothing to check it against yet. */
    if (!crypt_key_ready) {
        return KBD_CRYPT_INACTIVE;
    }
    if (!kbd_crypt_try_claim()) {
        return KBD_CRYPT_BUSY;
    }

    sid = (uint32_t)frame[2] | ((uint32_t)frame[3] << 8) |
          ((uint32_t)frame[4] << 16) | ((uint32_t)frame[5] << 24);

    /* Direction 0x02, counter 0: disjoint from every kb->dongle data nonce
     * (different direction byte) and unique across sessions (session_id). */
    build_nonce(nonce, sid, KBD_CRYPT_DIR_DONGLE_TO_KB, 0u);
    aad[0] = frame[0];
    aad[1] = KBD_CRYPT_TAG_SESSION;

    /* Empty payload: no message block, per the CCM construction above. */
    if (cbc_mac_b0(nonce, 2u, 0u, x) != HAL_AES_OK ||
        cbc_mac_aad(x, aad, 2u) != HAL_AES_OK ||
        ctr_block(nonce, 0u, s0) != HAL_AES_OK) {
        kbd_crypt_release();
        return KBD_CRYPT_FAULT_ENGINE;
    }
    kbd_crypt_release();

    /* Accumulate rather than early-exit, so the comparison does not leak how
     * many bytes matched. */
    for (i = 0; i < KBD_CRYPT_TAG_BYTES; i++) {
        diff |= (uint8_t)((x[i] ^ s0[i]) ^ frame[6 + i]);
    }
    if (diff != 0u) {
        return KBD_CRYPT_MAC;
    }

    *out_session_id = sid;
    return KBD_CRYPT_OK;
}

#if KBD_CRYPT_BENCH_KEY
/* ---------------------------------------------------- bench self-verify
 * See the header block for what this is hunting. */

void kbd_crypt_bench_snapshot(const uint8_t *frame, uint8_t len)
{
    uint8_t i;

    if (len < 16u || len > KBD_CRYPT_LEN_BOOT_KBD) {
        return;
    }
    for (i = 0; i < len; i++) {
        bench_pend_frame[i] = frame[i];
    }
    bench_pend_len = len;
    bench_pend_session = crypt_session_id;
    bench_pend_seal_bb = kbd_crypt_seal_bb;
    bench_pend_valid = 1u;
}

void kbd_crypt_bench_verify_pending(void)
{
    uint8_t nonce[13];
    uint8_t s0[16];
    uint8_t s1[16];
    uint8_t x[16];
    uint8_t aad[2];
    uint8_t plain[KBD_CRYPT_MAX_BODY];
    uint8_t good[KBD_CRYPT_TAG_BYTES];
    uint32_t ctr;
    uint8_t n;
    uint8_t diff;
    uint8_t i;

    if (!bench_pend_valid || !kbd_crypt_selfck_enable) {
        return;
    }
    n = (uint8_t)(bench_pend_len - KBD_CRYPT_FRAME_OVERHEAD);
    if (n == 0u || n > KBD_CRYPT_MAX_BODY) {
        bench_pend_valid = 0u;
        return;
    }
    if (!kbd_crypt_try_claim()) {
        return;                     /* engine busy: retry on the next arm */
    }
    bench_pend_valid = 0u;
    kbd_crypt_in_aes = 1u;          /* count BB overlap during the verify too */

    ctr = (uint32_t)bench_pend_frame[2]
        | ((uint32_t)bench_pend_frame[3] << 8)
        | ((uint32_t)bench_pend_frame[4] << 16)
        | ((uint32_t)bench_pend_frame[5] << 24);
    build_nonce(nonce, bench_pend_session, KBD_CRYPT_DIR_KB_TO_DONGLE, ctr);
    aad[0] = bench_pend_frame[0];
    aad[1] = bench_pend_frame[1];

    if (ctr_block(nonce, 1u, s1) != HAL_AES_OK) {
        goto out;
    }
    for (i = 0; i < n; i++) {
        plain[i] = (uint8_t)(bench_pend_frame[6 + i] ^ s1[i]);
    }
    if (cbc_mac_b0(nonce, 2u, n, x) != HAL_AES_OK ||
        cbc_mac_aad(x, aad, 2u) != HAL_AES_OK ||
        cbc_mac_msg(x, plain, n) != HAL_AES_OK ||
        ctr_block(nonce, 0u, s0) != HAL_AES_OK) {
        goto out;
    }
    diff = 0u;
    for (i = 0; i < KBD_CRYPT_TAG_BYTES; i++) {
        good[i] = (uint8_t)(x[i] ^ s0[i]);
        diff |= (uint8_t)(good[i] ^ bench_pend_frame[6 + n + i]);
    }
    if (diff == 0u) {
        kbd_crypt_selfck_ok++;
    } else {
        kbd_crypt_selfck_bad++;
        if (!kbd_crypt_selfck_latched) {
            kbd_crypt_selfck_len = bench_pend_len;
            kbd_crypt_selfck_session = bench_pend_session;
            kbd_crypt_selfck_seal_bb = bench_pend_seal_bb;
            for (i = 0; i < bench_pend_len; i++) {
                kbd_crypt_selfck_frame[i] = bench_pend_frame[i];
            }
            for (i = 0; i < KBD_CRYPT_TAG_BYTES; i++) {
                kbd_crypt_selfck_good[i] = good[i];
                kbd_crypt_selfck_s1[i] = s1[i];
                kbd_crypt_selfck_s0[i] = s0[i];
            }
            for (i = 0; i < n; i++) {
                kbd_crypt_selfck_plain[i] = plain[i];
            }
            kbd_crypt_selfck_latched = 1u;
        }
    }
out:
    kbd_crypt_in_aes = 0u;
    kbd_crypt_release();
}
#endif /* KBD_CRYPT_BENCH_KEY */
