# Link encryption — where we are, and what to do next

Branch `firmware-link-encryption`. Companion receiver work: OpenDongle branch
`em-ccm-bench-verify` (`6d5aa87`).

One open defect blocks the feature: **on an encrypted bond, ~12% of sealed
frames fail their CCM tag at the receiver, and two consecutive failures release
the link.** Everything else is built and proven. The next step is one small,
fully-specified receiver change (§4).

---

## 1. Proven on hardware

The keyboard transmit path works end to end. A receiver that forwards nothing to
USB without a passing CCM tag delivers keystrokes from this firmware — that
delivery *is* the proof.

Measured over five probe-free connect cycles:

| | |
|---|---|
| Keyboard sealed | 277 (`seal_miss` 0, `sess_bad` 0) |
| Receiver saw arriving | 276 — agreement within one in-flight frame |
| Verified | 242 |
| MAC failures | 34 (**12.3%**) |
| `replay` / `shape` / `inactive` / `fifo_full` / `flush_drop` | all 0 |
| Largest LEN seen | 22, tag `0xA1` — frames arrive intact and correctly shaped |

Also: 32 host tests pass against OpenDongle's `ccm_ref.py` golden corpus plus the
pinned on-silicon vector, and the `KBD_RF_CRYPT=0` build stays **byte-identical**
to the validated plaintext image (`sha256 ccdfa519…b47e4b2`), so the feature gate
is genuinely zero-impact.

## 2. The disconnect chain — solved and quantified

Derived from source and confirmed on hardware:

- Poll period = `conn_interval 28 / 32000` = **0.875 ms**
- Keepalive = 32 polls = **28 ms**
- Silence guard = 64 receptions with nothing verified = **56 ms** — exactly two
  keepalive opportunities
- **Two *consecutive* failed keepalives are required.** `RF_EVT_CRYPT_RX` is
  dispatched before `RF_EVT_POLL`, so a keepalive that verifies at reception 64
  resets the counter before the guard is checked.
- Keyboard supervision = 5000 TMOS ticks × 625 µs = **3.125 s**, then `5B 33`

Expected trials to two consecutive failures is `(1+q)/q²`:

| MAC rate | guard fires | + 3.125 s → `5B 33` |
|---:|---:|---:|
| 8% | 4.73 s | 7.85 s |
| 10% | 3.08 s | 6.21 s |
| 12% | 2.18 s | 5.30 s |

Observed 6–8 s, and a measured run gave connect → drop of 3.46 s against the
3.125 s prediction. This chain is **not** a defect to fix on its own; it is the
correct response to frames not verifying. Fix the MAC failures and it goes away.

*(Open question worth revisiting afterwards: the guard tolerates only one failed
keepalive. Even at a healthy failure rate that is thin margin for a radio link,
and the two constants live in different repos with nothing tying them together.)*

## 3. The open defect

**~12% of sealed frames fail their tag**, with key, session, counter and frame
shape all provably correct. Nothing is lost in transit — the keyboard's sealed
count and the receiver's arrival count agree exactly.

The one strong clue, unexplained:

| condition | frames | verified | MAC failures | rate |
|---|---|---|---|---|
| idle (keepalives only) | 276 | 242 | 34 | **12.3%** |
| heavy HID traffic | 391 | 377 | 14 | **3.6%** |

More crypto activity produces *fewer* failures. No mechanism yet predicts that
3.4× ratio.

## 4. NEXT STEP — the experiment to run

On each receiver `DROP_MAC`, immediately re-run the tag computation **under the
same current session**: same counter, same AAD, same ciphertext, a separate
scratch body, side-effect free (never advance `rf_crypt_last_ctr`, never rescue
the frame). Count `rf_crypt_mac_same_ok`.

- Add beside the existing diagnostic counters in `OpenDongle/firmware/common/src/rf_crypt.c`.
- Insert at the first `diff != 0`, ahead of the previous-session retry.
- `CMD_CRYPT_DIAG` is already exactly 64 bytes — reuse the now-spent
  `mac_prev_ok` field rather than appending.

Interpretation:

- `mac_same_ok > 0` → the identical received bytes verify on a second attempt ⇒
  **receiver-side** transient computation failure.
- `mac_same_ok == 0` across dozens of failures ⇒ the receiver deterministically
  rejects the same bytes twice ⇒ fault is in **keyboard sealing**.

Every measurement so far has assumed the receiver computes correctly. This is the
first test that checks it, and it splits the search space in half. It runs only
on actual failures and does not perturb keyboard timing.

## 5. Refuted — do not re-test

Each of these cost real time. They are settled.

- **Stale session / nonce churn.** 0 of 34 MAC failures verify under the
  displaced session id, and `session_mint_count` delta was 0 across a full run.
  Re-mints happen only at connect and EV10 re-promotion, never mid-epoch.
- **Frames lost before the crypto path.** `fifo_full` and `flush_drop` are 0;
  the two blind spots in the original telemetry are measured, not inferred.
- **Receiver never entering `rf_crypt_rx()`.** It does; 242 frames verified.
- **RF bit errors.** CRC rejection provably precedes the crypto path
  (`rsr != 0` → CRCERR, sink exits before classification), so noise cannot
  produce a MAC-only failure rate.
- **The keyboard rebooting / a periodic HardFault.** Entirely a debug-probe
  artifact — see §6.
- **PHY/DMA RX length cap.** The vendor lib replaces an unset `RxMaxlen` with
  251 (confirmed by disassembly); `rxBuf` is `MEM_BUF` with 544 B before the heap.
- **Body mutating between `seal_begin` and `seal_finish`.** `seal_begin`
  snapshots the body into local `plain[]` before any AES; `seal_finish` never
  reads the live source.
- **Counter accumulation / wrap.** `crypt_tx_ctr` advances once per successful
  seal, gaps are permitted by design, wrap is rejected at `0xFFFFFFFF`.
- **ISR race on `seal_*` state.** Only reachable under
  `STOCK_ISR_FAST_RESPONSE=1`; this build is `0`, so both begin and finish run
  in cooperative task context.
- **"~32 seal_begin calls per idle frame."** My model, and it was wrong: the ARM
  handler refuses to re-seal while one is pending, so it is one seal per
  transmitted frame idle and loaded alike.
- **AES/BLEB engine contention** — *demoted, not excluded.* The output asymmetry
  fits beautifully (10 of 11 corrupted AES blocks give "ciphertext correct, tag
  wrong"), and `RF_EVT_CRYPT_ARM` really is posted before the asynchronous
  `RF_Tx()`, so a ~160 µs seal can overlap an ~88 µs transmission. But the linked
  `BB_IRQLibHandler` only reads and clears `AES_STA`, never writing CFG/KEY/DATA,
  and our HAL polls CFG — so no verified code clobbers the engine. It also fails
  to predict the load asymmetry.

## 6. Methodology — the expensive lesson

**Never build a rate measurement on repeated debug-probe attaches.**

A long investigation concluded the keyboard was HardFaulting every few seconds.
It never rebooted at all. `minichlink`'s attach resets a running application, so
every sampling loop that polled once per iteration produced a "reboot rate" that
simply tracked the *polling* rate — 1/60 s when polling every 60 s, 1/2.3 s when
polling every 2.3 s. The control: leave the probe untouched and read once.
Advance is **exactly +1 per attach, independent of elapsed time** (checked at
2.3 s, 60 s, 90 s, 300 s gaps).

Two compounding traps:

- "Four back-to-back reads showed no increment" looked exculpatory but was the
  opposite — consecutive rapid attaches share one debug session, so rapid-fire
  reads were the one access pattern that *could not* reproduce the artifact.
- `firmware/README.md` also documents that minichlink CH5xx memory reads are
  unreliable (a valid SRAM address can read back all zeros). An all-zero counter
  read was taken as evidence of a reboot — precisely that failure mode. So the
  false conclusion had two independent bad foundations.

The sound channels, both in place now:

- Receiver: `CMD_CRYPT_DIAG` (IAP `0x94`) over USB.
- Keyboard: `0xAF` → `[0x5D][5 × u32 LE][checksum]` over UART (`a2ed588`).

WCH OpenOCD does **not** help here: per the same README, attaching to a running
application resets it, so it cannot observe live state either. Firmware
instrumentation read out of band is the only sound approach.

## 7. Other open items

- **Receiver wedges.** The CH592 stopped answering IAP three times in one
  session, twice dropping off USB entirely; only a probe reset recovers it. This
  gates measurement — runs die partway. No credible mechanism found in the
  `DONGLE_RF_CRYPT` path; the bounded HAL and sparse traffic argue against
  crypto starvation. Needs its own investigation.
- **Capability advert never lands** — every pair records `capable=no`, and
  `bond_enc_active()` needs `capable AND key`. Bench uses
  `provision_link_key.py`, which sets both. A beacon-chase fix was tried and
  **broke pairing entirely** (the keyboard goes deaf transmitting the advert
  exactly when the ACK arrives) — do not repeat it.
- **Session announce is fire-and-forget.** The receiver announces a new session
  up to 8 times and never again until the next connect/EV10. A keyboard that
  misses all 8 (e.g. keyed after the mint) can never seal for that whole epoch.
  Observed directly: provisioning keys after pairing produced runs with zero
  encrypted frames.
- **CCM counter persistence.** The per-boot random start (`4ef00be`) makes
  keystream reuse unlikely but does not guarantee it. Reserved counter ranges in
  flash, or per-session keys, is the real answer.
- **Phase 2 (ECDH key establishment) not started.** The link key is still a
  bench scaffold behind `KBD_CRYPT_BENCH_KEY`, which must never ship enabled.
  X25519 design is in the approved plan.
- **`fault_handler.S` comment is wrong** — it claims trap entry clears
  `mstatus.MIE`, but QingKe V4 does not (nesting is supported by design). It
  should not claim atomicity it lacks.
- **CH570 build is broken on the OpenDongle bench branch** — fails its
  stack-floor assert, pre-existing and unrelated to the diagnostics added there.

## 8. Bench recipes

**Pairing order is load-bearing.** Select the transport (`A6 30`), put the
KEYBOARD into pairing FIRST (`A6 51`), and only then restart the dongle — it
accepts a pair only in the first few seconds after boot, so its window must open
while the keyboard is already broadcasting. Same ordering for a bonded
reconnect. Getting this backwards explains every "the CH592 refuses to pair"
observation in this project.

**Provision keys BEFORE pairing**, or the keyboard cannot verify the announces
from the mint that happens at connect (§7).

Update over OBP, not `flash-factory` — **the bond survives an OBP update** and a
factory flash erases it. Retry through two transients rather than racing them:
the device re-enumerates entering the bootloader, and udev takes a moment to
apply the plugdev ACL. Retry on both "no HID device" and "Permission denied";
once any command completes a HELLO the bootloader is fail-stay.

**Pass the feature knobs to `update`, not just `bundle`** — `make update`
rebuilds, so `make KBD_RF_CRYPT=1 ... update` is required or it silently flashes
a default (plaintext) image.

IAP packets are `[report-id 0][cmd][len][body][checksum]` padded to 65 bytes,
`checksum = (cmd + len + sum(body)) & 0xFF`. An unchecksummed packet is ignored
**silently** — the symptom is a bare timeout. `BondRead` replies
`[ack][len][status][record…]`, so the record starts at offset **3**; reading from
2 shifts every field and yields plausible-but-wrong values. The link key is
redacted by design.

`.diag_safe` addresses shift whenever a counter is added — always re-derive from
the current `.map`. `rf_last_tx_status` / `rx_status` / `config_status` are
packed `uint8_t`, not words.

Bench scripts live in `firmware/bench/` (see its README). Probes: keyboard
`CEBD8F0653EF`, CH592 dongle `C2228F064754`.

## 9. References

- `firmware/docs/LINK-ENCRYPTION-STATUS.md` — feature status and design notes
- `firmware/docs/CRASH-DEBUG-PLAN.md` (branch `firmware-crash-debug`) — the
  refuted HardFault investigation, retained as a record; **its premise is void**
- Codex sessions: `019feb15-3314-7e71-a94e-bc9453eabea6` (first AES review),
  `019febc6-c8a4-78b2-b5c0-bca2072f95fb` (adversarial review of the crash plan),
  `019fec05-2531-7ae3-9869-139bf780fd68` (disconnect chain arithmetic),
  `019fec41-5126-7603-bf11-17a7ad62f579` (AES asymmetry + the §4 experiment)
