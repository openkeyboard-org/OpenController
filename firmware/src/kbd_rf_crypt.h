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
 * That is worth the extra block encryptions. It keeps the 100 us
 * poll-to-response turnaround free of crypto entirely, and it removes any
 * possibility of the response path racing main-loop use of a cipher that is
 * explicitly not reentrant. The extra work lands where there is no deadline.
 *
 * The cost figures here USED to say "eight extra block encryptions" and
 * "~160 us per report". Both predate the seal double-compute
 * (kbd_crypt_seal_begin computes the whole seal twice and requires the passes
 * to agree). One pass is B0 + CTRL_VARIANTS x (AAD + MSG) + two CTR blocks
 * = 1 + 4x2 + 2 = 11 blocks, so a normal seal is now ~22 and a caught
 * disagreement (kbd_crypt_seal_redo) costs another pass on top.
 * [MEASUREMENT REQUIRED] for the microsecond figure -- do not quote the old
 * ~160 us, it is roughly half the current work.
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

/* Pairing slot policy, factored out of rf_task.c so it is host-testable
 * (firmware/tests/test_pair_slots.py). rf_task.c must hold no second copy.
 *
 * Each 20 ms pair slot carries the advert FIRST and its beacon a lead later, so
 * the first frame a receiver hears is an advert wherever in the stream it joins.
 * The receiver commits the bond on the first BEACON and never re-arms RX on the
 * pair AA before promoting, so an advert that follows a beacon is unreachable --
 * which is why the old "slots 0,1 then one in eight" schedule latched
 * capability 0/10 in the documented pairing order.
 *
 * P4, the property that must never regress: nothing is transmitted in the quiet
 * window immediately AFTER a beacon, because that is where the pair-ACK arrives.
 * An advert placed there left the keyboard deaf to the ACK and broke pairing
 * outright (2026-08). */
#define KBD_PAIR_BCAST_TICKS       32u   /* 20 ms at 625 us/tick */
#define KBD_PAIR_ADVERT_LEAD_TICKS  4u   /* 2.5 ms: advert -> its own beacon */
#define KBD_PAIR_DWELL_BCASTS      12u   /* beacons per channel dwell */

/* Advance one pair slot. Sets *send_advert and returns the tick delay until the
 * next PAIR_BCAST event. `advert_enabled` is false for a bonded reconnect (whose
 * capability is already on the receiver's record) and for a plaintext build. */
static inline uint16_t kbd_pair_slot_next(uint8_t advert_enabled,
                                          uint8_t *slot_phase,
                                          uint8_t *send_advert)
{
    if (advert_enabled && *slot_phase == 0u) {
        *slot_phase = 1u;
        *send_advert = 1u;
        return KBD_PAIR_ADVERT_LEAD_TICKS;
    }
    *slot_phase = 0u;
    *send_advert = 0u;
    return (uint16_t)(KBD_PAIR_BCAST_TICKS
                      - (advert_enabled ? KBD_PAIR_ADVERT_LEAD_TICKS : 0u));
}

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
 * session: a new key means the old session_id/counter pair is meaningless.
 *
 * `ctr_start` is the last-consumed counter, so the first frame transmitted uses
 * ctr_start + 1. Pass a value that VARIES per boot rather than 0.
 *
 * Why it matters: the nonce is session_id || direction || counter, so reuse
 * needs both halves to repeat. The receiver mints session_id from a xorshift32
 * seeded off chip UID and RTC, which its own source notes is close to
 * deterministic for a fixed device once the SRAM-PUF contribution is compiled
 * out -- and on this bench its session AA came back identical across reboots.
 * With both ends restarting deterministically, one repeated session_id gives
 * GUARANTEED keystream reuse across a reboot. A varying start turns that into
 * "session repeats AND the two counter ranges overlap", which is negligible.
 *
 * It does NOT need to be unpredictable -- the counter is transmitted in the
 * clear, and CCM requires nonce uniqueness, not nonce secrecy. Mixing chip UID,
 * RTC and SysTick is therefore sufficient here, even though the same sources
 * would be quite inadequate for, say, a key.
 *
 * Keep it inside KBD_CRYPT_CTR_START_MAX. Starting near the top of the range
 * leaves too few counters before exhaustion: at the keepalive's ~36 frames/s a
 * few hundred remaining values last seconds. */
void kbd_crypt_install_key(const uint8_t key[KBD_CRYPT_KEY_BYTES],
                           uint32_t ctr_start);

/* Bound on the counter, leaving >= 2^31 values -- about two years of continuous
 * transmission at the keepalive rate. A start at or above this is CLAMPED (to
 * this value minus one, since the stored counter is the last one consumed and
 * the next seal transmits +1, so the first transmitted counter stays <= this
 * bound). Clamped, not masked: masking turned a start just past the bound into
 * a near-zero one. */
#define KBD_CRYPT_CTR_START_MAX 0x7FFFFFFFu

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

/* Seal double-compute disagreements: radio/AES collisions caught (and the
 * seal recomputed) before a corrupted block could poison a frame. See the
 * note inside kbd_crypt_seal_begin(). */
extern uint32_t kbd_crypt_seal_redo;

/* Throw away a prepared frame without building another. Cipher-free, so it is
 * safe anywhere -- unlike seal_begin(), whose ~160 us of AES must not run at an
 * arbitrary moment (see the note on seal_begin's cost). Use it when the report a
 * prepared frame carries has been superseded: discard here, and let the next
 * arm build the replacement in a slot chosen for it. */
void kbd_crypt_seal_discard(void);

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

#if KBD_CRYPT_BENCH_KEY
/* --------------------------------------------------------- bench self-verify
 *
 * Bench evidence (2026-08-15, offline ccm_ref oracle on a receiver-latched
 * frame): the keyboard seals a CORRECT ciphertext under a GARBAGE tag for
 * ~12% of idle keepalives -- the corruption is confined to the tag pipeline
 * (s0/B0/AAD/MSG blocks of seal_begin), while the s1 keystream block of the
 * SAME seal computes correctly, and the receiver's compute is exonerated
 * (deterministic, KAT-clean, oracle-identical). These hooks catch it at the
 * source: rf_task snapshots each transmitted sealed frame, and the next
 * CRYPT_ARM re-derives the tag from the frame bytes in task context.
 *
 *   selfck_bad > 0            => the bad tag is observable ON the keyboard at
 *                                seal time (engine/overlap corruption);
 *   selfck_bad == 0 while the receiver still sees DROP_MAC => the bytes
 *                                mutate AFTER sealing (buffer/DMA path).
 *
 * bb_during_aes counts RF status callbacks that landed inside a seal/verify
 * (the vendor IRQ path touches AES_STA); seal_bb latches the count for the
 * seal whose frame first fails self-verify. */
extern volatile uint8_t kbd_crypt_in_aes;
extern uint32_t kbd_crypt_bb_during_aes;
extern volatile uint8_t kbd_crypt_seal_bb;       /* callbacks in CURRENT seal */
extern uint32_t kbd_crypt_selfck_ok;
extern uint32_t kbd_crypt_selfck_bad;
extern uint8_t  kbd_crypt_selfck_latched;
extern uint8_t  kbd_crypt_selfck_len;
extern uint32_t kbd_crypt_selfck_session;
extern uint8_t  kbd_crypt_selfck_frame[KBD_CRYPT_LEN_BOOT_KBD];
extern uint8_t  kbd_crypt_selfck_good[KBD_CRYPT_TAG_BYTES];  /* recomputed tag */
extern uint8_t  kbd_crypt_selfck_seal_bb;        /* seal_bb of the latched seal */
extern uint8_t  kbd_crypt_selfck_plain[KBD_CRYPT_MAX_BODY]; /* verify's plain */
extern uint8_t  kbd_crypt_selfck_s1[8];          /* verify's keystream block */
extern uint8_t  kbd_crypt_selfck_s0[8];          /* verify's tag-mask block */

/* Runtime enable for the pre-seal verify (default ON in bench builds).
 * Toggled over UART 0xB1 -- flipping it mid-session with no reboot is the
 * A/B for "the verify's ~73 us timing shift masks the MAC-failure defect". */
extern volatile uint8_t kbd_crypt_selfck_enable;

/* Snapshot the frame just handed to RF_Tx (call right after seal_finish OK;
 * copies the bytes + the live session id + the seal's BB count). */
void kbd_crypt_bench_snapshot(const uint8_t *frame, uint8_t len);

/* If a snapshot is pending, re-derive its tag from the frame bytes (full
 * pipeline, task context, engine claimed) and count/latch the outcome.
 * Call from CRYPT_ARM before the next seal_begin. Side-effect free on all
 * crypto state. */
void kbd_crypt_bench_verify_pending(void);
#endif /* KBD_CRYPT_BENCH_KEY */

#endif /* KBD_RF_CRYPT_H */
