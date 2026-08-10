/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 *
 * AES-128-CCM link encryption for the keyboard->dongle HID uplink.
 *
 * This is the TRANSMIT peer of OpenDongle's `firmware/common/src/rf_crypt.c`
 * (branch `em-rf-ccm-rx-decrypt`), which decrypts what this produces. The two
 * must agree byte-for-byte; both are graded against the same host reference,
 * OpenDongle's `firmware/tests/ccm_ref.py`, which is itself graded against
 * RFC 3610.
 *
 * WIRE FORMAT (L=2 CCM, 13-byte nonce, 8-byte tag). Bytes as transmitted, i.e.
 * the radio payload after the hardware LEN byte -- the receiver sees this at
 * rxBuf[2] onward and `len` at rxBuf[1]:
 *
 *   [ctrl][tag][counter:u32 LE][ciphertext(body)][mic:8]      LEN = body + 14
 *
 *   nonce = session_id(4 LE) || direction(1) || counter(4 LE) || 0,0,0,0
 *   AAD   = ctrl || tag        (transmitted in clear, authenticated)
 *
 * The encrypted LEN values {16,19,22} are deliberately disjoint from the
 * plaintext set {1,3,4,7,10,15}, so tag+length classification still routes on
 * the receiver.
 *
 * THE CTRL BYTE IS AAD, AND IT IS NOT KNOWN UNTIL THE POLL ARRIVES. It carries
 * the link's ARQ sequencing and is latched from the received poll (rf_task.c),
 * so a frame cannot be sealed against one particular ctrl ahead of time.
 *
 * It can be sealed against ALL of them, which is what this module does. Only
 * two bits of ctrl ever vary: rf_task.c seeds tx_ctrl once per connection and
 * thereafter only replaces bit 0 and toggles bit 1, never touching the rest.
 * Four candidate tags therefore cover every value the ARQ logic can produce.
 * kbd_crypt_seal_begin() computes all four in main-loop context, and
 * kbd_crypt_seal_finish() -- which runs in the response path -- selects one and
 * emits the frame WITHOUT CALLING THE AES ENGINE AT ALL.
 *
 * That is worth the eight extra block encryptions. It keeps the 100 us
 * poll-to-response turnaround free of crypto entirely, and it removes any
 * possibility of the response path racing main-loop use of a cipher that is
 * explicitly not reentrant. The extra work lands where there is no deadline:
 * ~160 us per report against a poll interval of ~17.5 ms.
 *
 * COUNTER DISCIPLINE -- two rules, both load-bearing for nonce uniqueness.
 *
 * It increments per TRANSMISSION, not per report. The link retransmits an
 * unchanged report several times (HID_RESEND_COUNT) and each retransmission may
 * carry a different ctrl, so reusing one counter across them would authenticate
 * different AAD under one nonce -- a real CCM nonce-reuse weakening, since both
 * tags share S_0 and their XOR leaks the MAC difference. A retransmission after
 * a lost frame still validates, because the receiver's high-water mark only
 * advances on a frame that verified.
 *
 * It is MONOTONIC FOR THE LIFETIME OF THE KEY, not per session: adopting a
 * session does NOT restart it. Restarting would make nonce uniqueness rest on
 * session_id never repeating, which it does -- both in normal operation (the
 * receiver re-announces one session up to 8 times, and a later announcement can
 * land after we have already transmitted under it) and under replay (a session
 * frame carries no freshness, so a recorded one stays valid forever). See
 * kbd_crypt_adopt_session() for the full argument. The counter starts at 1 for
 * a freshly installed key; 0 is reserved and the receiver rejects it.
 *
 * CONTEXT. hal_aes is NOT reentrant (one shared engine, module state). Every
 * entry point here that touches it claims the engine and returns
 * KBD_CRYPT_BUSY rather than proceeding, so the contract is enforced instead of
 * assumed. kbd_crypt_seal_finish() needs no claim because it does no crypto.
 */
#ifndef KBD_RF_CRYPT_H
#define KBD_RF_CRYPT_H

#include <stdint.h>

#define KBD_CRYPT_KEY_BYTES       16u
#define KBD_CRYPT_TAG_BYTES        8u   /* CCM MIC, M=8 */
#define KBD_CRYPT_CTR_BYTES        4u
#define KBD_CRYPT_MAX_BODY         8u   /* boot-keyboard report, the largest */
#define KBD_CRYPT_FRAME_OVERHEAD  14u   /* ctrl+tag+counter+mic */
#define KBD_CRYPT_MAX_FRAME       (KBD_CRYPT_MAX_BODY + KBD_CRYPT_FRAME_OVERHEAD)

/* The ctrl bits the ARQ logic varies, and hence how many candidate tags a seal
 * must carry. Keep in step with rf_task.c's tx_ctrl update. */
#define KBD_CRYPT_CTRL_VARIANT_MASK 0x03u
#define KBD_CRYPT_CTRL_VARIANTS      4u

/* How often an idle link must put an AUTHENTICATED frame on air, in connected
 * receptions. The receiver force-releases after 64 receptions with nothing
 * verified among them (RF_CRYPT_SILENCE_FRAMES) and counts bare poll acks
 * toward it, so an idle keyboard that only acked would be dropped in ~56 ms.
 *
 * 32 (~28 ms against a ~56 ms deadline) is what has been observed delivering
 * HID on hardware. It is NOT yet a settled number: an idle link survived one
 * 8 s soak at this value and dropped after ~2 s on the next, so the keepalive
 * does not reliably hold an idle encrypted link yet. Tightening it to 16 made
 * matters worse rather than better -- keystroke delivery stopped entirely --
 * which points at the keepalive contending with real reports for the single
 * pre-sealed frame rather than at the margin being too thin. Unresolved; see
 * the bench notes before changing this. */
#define KBD_CRYPT_KEEPALIVE_POLLS   32u

/* Direction byte folded into the nonce, domain-separating the link halves. */
#define KBD_CRYPT_DIR_KB_TO_DONGLE 0x01u
#define KBD_CRYPT_DIR_DONGLE_TO_KB 0x02u

/* Report tags and their encrypted on-air LEN. Only the boot keyboard is
 * transmitted by this firmware today; the others are the receiver's contract
 * and are kept so the length table stays complete. */
#define KBD_CRYPT_TAG_BOOT_KBD    0xA1u
#define KBD_CRYPT_LEN_BOOT_KBD     22u  /* body 8 */
#define KBD_CRYPT_TAG_CONSUMER    0xA3u
#define KBD_CRYPT_LEN_CONSUMER     16u  /* body 2 */
#define KBD_CRYPT_TAG_MOUSE       0xA8u
#define KBD_CRYPT_LEN_MOUSE        19u  /* body 5 */

/* Session-nonce control frame (dongle->keyboard), authenticated, empty payload:
 * [ctrl][0xA5][session_id:u32 LE][mic:8]. */
#define KBD_CRYPT_TAG_SESSION     0xA5u
#define KBD_CRYPT_LEN_SESSION      14u

/* Capability advert (keyboard->dongle, at pairing): [ctrl][0xA6][version].
 * Purely additive -- a receiver that does not know it ignores it. */
#define KBD_CRYPT_TAG_CAP         0xA6u
#define KBD_CRYPT_LEN_CAP           3u
#define KBD_CRYPT_CAP_VERSION       1u

typedef enum {
    KBD_CRYPT_OK = 0,
    KBD_CRYPT_INACTIVE,       /* no key and/or no session installed */
    KBD_CRYPT_SHAPE,          /* not a well-formed frame for this call */
    KBD_CRYPT_MAC,            /* tag mismatch (forgery, noise, wrong session) */
    KBD_CRYPT_EXHAUSTED,      /* counter space used up; re-key required */
    KBD_CRYPT_BUSY,           /* engine claimed by the other context */
    KBD_CRYPT_FAULT_ENGINE    /* AES engine wedged -- terminal, see hal_aes.h */
} kbd_crypt_status_t;

/* ---------------------------------------------------------------- lifecycle */

/* Bring the AES backend up. Task/boot context. Safe to call more than once. */
void kbd_crypt_init(void);

/* Install the 16-byte link key (runs the key schedule once). Clears any
 * session: a new key means the old session_id/counter pair is meaningless. */
void kbd_crypt_install_key(const uint8_t key[KBD_CRYPT_KEY_BYTES]);

/* Adopt a session_id. Call ONLY after kbd_crypt_verify_session() has
 * authenticated the frame that carried it. Does NOT reset the TX counter --
 * see the counter discipline above and the argument at the definition. */
void kbd_crypt_adopt_session(uint32_t session_id);

/* Drop the session but keep the key (e.g. on link loss): seals fail with
 * KBD_CRYPT_INACTIVE until the next authenticated session frame. */
void kbd_crypt_end_session(void);

/* Zeroize the key schedule and clear all module state. */
void kbd_crypt_clear(void);

/* Non-zero once a key AND a session are installed. */
int kbd_crypt_active(void);

/* Non-zero if a key is installed, regardless of session state. */
int kbd_crypt_keyed(void);

/* The session currently adopted (undefined unless kbd_crypt_active()). */
uint32_t kbd_crypt_session_id(void);

/* ------------------------------------------------------- engine serialization
 *
 * Used internally to enforce hal_aes's non-reentrancy; exposed so a caller can
 * hold the engine across a sequence if it ever needs to. A failed claim means
 * another context holds it. */
int  kbd_crypt_try_claim(void);
void kbd_crypt_release(void);

/* ------------------------------------------------------------------ transmit
 *
 * Sealing is split so that no AES work lands in the response path.
 *
 * kbd_crypt_seal_begin() runs in main-loop context, consumes one counter value,
 * and computes the keystream, the ciphertext, and a finished MAC for every
 * reachable ctrl value. `ctrl_hint` supplies the invariant high bits of ctrl
 * (pass the current tx_ctrl); only the low KBD_CRYPT_CTRL_VARIANT_MASK bits are
 * enumerated.
 *
 * kbd_crypt_seal_finish() selects the MAC matching the ctrl the poll actually
 * carried and emits the frame. It performs NO cipher operations, so it is safe
 * and cheap in the turnaround. It returns KBD_CRYPT_SHAPE if ctrl falls outside
 * the enumerated set -- the caller should then send its plain keepalive rather
 * than anything derived from unsealed data.
 *
 * A begin() must be followed by exactly one finish(); a second finish() without
 * an intervening begin() returns KBD_CRYPT_SHAPE rather than resending a frame
 * under an already-used counter. */
kbd_crypt_status_t kbd_crypt_seal_begin(uint8_t ctrl_hint, uint8_t tag,
                                        const uint8_t *body, uint8_t body_len);

kbd_crypt_status_t kbd_crypt_seal_finish(uint8_t ctrl,
                                         uint8_t *out, uint8_t *out_len);

/* Non-zero if a seal_begin() result is waiting for its seal_finish(). */
int kbd_crypt_seal_pending(void);

/* One-shot seal for callers with no timing constraint (host tests, and any
 * path that already runs in task context). Equivalent to begin()+finish(). */
kbd_crypt_status_t kbd_crypt_seal(uint8_t ctrl, uint8_t tag,
                                  const uint8_t *body, uint8_t body_len,
                                  uint8_t *out, uint8_t *out_len);

/* ------------------------------------------------------------------- receive
 *
 * Authenticate a LEN-14 session frame WITHOUT adopting it. `frame` points at
 * the payload (the receiver's rxBuf[2] equivalent, i.e. [ctrl][0xA5][sid][mic])
 * and `len` is the on-air LEN. On KBD_CRYPT_OK the frame is genuine and
 * *out_session_id holds the carried session_id; the caller decides whether to
 * adopt it (see kbd_crypt_adopt_session).
 *
 * Verification requires the key only -- not an active session -- because the
 * session_id being authenticated is the one carried IN the frame: the nonce is
 * built from the candidate, so a tampered session_id yields a different nonce
 * and fails the tag. That is the only thing binding those cleartext bytes. */
kbd_crypt_status_t kbd_crypt_verify_session(const uint8_t *frame, uint8_t len,
                                            uint32_t *out_session_id);

/* Body length for a valid encrypted (tag, len) pair, else 0. Mirrors the
 * receiver's rf_crypt_encrypted_body_len(). */
uint8_t kbd_crypt_encrypted_body_len(uint8_t tag, uint8_t len);

#endif /* KBD_RF_CRYPT_H */
