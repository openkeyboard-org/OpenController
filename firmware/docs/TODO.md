# Link encryption — where we are, and what to do next

Branch `firmware-link-encryption`. Companion receiver work: OpenDongle branch
`em-ccm-bench-verify`.

**2026-08-16: the blocking defect is ROOT-CAUSED and the link runs clean.**
Two idle soaks measured **0 MAC failures over 33.5k and 20.8k keepalives**
(vs the historical 12.3%). See §0 for the mechanism and the caveat; the next
step is the structural fix it prescribes.

---

## 0. ROOT CAUSE (2026-08-16)

**The CH592's AES engine silently returns the PREVIOUS block's output when a
block operation is preempted by the BLEB radio interrupt** — CFG bit 0 reads
back clear ("complete"), but the computation never ran and the DATA read path
still holds stale output. (This differs from the mocked-register experiment,
where write and read share memory and an aborted op returns the *input*: real
silicon has separate in/out latches.) The keyboard's seal overlapped its own
`RF_Tx`/TX_FINISH window, so one interrupt per sealed frame occasionally landed
inside one of the seal's 11 block windows: that block yielded the previous
block's bytes — intact ciphertext (s1 computed early), garbage tag, and the
inverse idle/loaded asymmetry via phase shifts.

Evidence chain, all on hardware:
- Receiver exonerated: same-session re-verify `mac_same_ok` 0/34+, computed
  tags deterministic (`same_differs` 0), FIPS KAT passes, and the offline
  `ccm_ref.py` oracle reproduces the receiver's tag exactly.
- Keyboard convicted per-frame: latched failing frames decrypt to the correct
  all-zero keepalive body (ciphertext byte-identical to the oracle's seal)
  under a tag that verifies under NO ctrl variant.
- Mechanism caught in the act: a keyboard-side re-verify latched `x == s0`
  exactly (final XOR collapses the tag to zeros — the s0 block returned the
  CBC-MAC state still in the engine), at ~96% failure when one BB callback
  landed inside the computation and ~0% when none did.
- Codex disassembly of the linked LIBCH59xBLE.a v1.4.2: `BB_IRQLibHandler`
  reads `AES_STA`, clears bits 1 then 0 (only when bit 1 is set), never
  touches CFG/KEY/DATA, never calls `phy_status_clear`. Conclusion: BLEB
  handler preemption aborts the op; the exact register-level trigger inside
  the handler is not further separable from this repository.

**Why it currently measures 0%:** the bench self-verify
(`kbd_crypt_bench_verify_pending`, called at the top of `rf_crypt_arm`) adds
~73 µs of AES before each `seal_begin`, shifting the seal out of the hazardous
window. That is a timing accident doing the work of a fix — the verify itself
sits inside the TX window and reads ~100% "bad" (a sacrificial canary; its
counters do NOT indicate link health — the receiver's ok/mac counters are the
ground truth).

**The fix (as landed, two halves):**
1. Arm the seal from the TX_FINISH path instead of before `RF_Tx`, so
   `seal_begin` structurally cannot overlap the transmission it follows
   (`crypt_arm_after_tx` latch, consumed by the TX_FINISH/TX_FAIL callback
   with a `tx_status != 0` fallback). Alone this measured 0.5% residual —
   the poll grid keeps the radio active every 875 µs, so timing cannot fully
   close the window.
2. `seal_begin` derives every block TWICE and requires the passes to agree
   (stale-abort output cannot survive an honest recompute); one bounded
   retry, then `KBD_CRYPT_BUSY` — bare ack this slot, session survives,
   next arm retries. `kbd_crypt_seal_redo` counts collisions caught.

**Validated 2026-08-16**, 300 s idle soaks, fresh pair each: verify-on
10459/10459 and verify-off 10200/10200 sealed frames verified, **0 MAC
failures**, while `seal_redo` caught 742 and 2273 live collisions
respectively — the redo rate rises when the pre-seal verify's ~73 µs is
removed (seal starts earlier in the active window) while the on-air rate
stays zero: the dose-response that confirms both mechanism and guard. The
link no longer depends on any timing accident; the bench self-verify is
now diagnostics only.

**Tried and withdrawn:** masking `mstatus.MIE` across each
`hal_aes_encrypt_block` as defense in depth. Architecturally sound per the
Codex review (VTF/HPE interrupts are MIE-maskable on QingKe V4C; WCH's own
RTOS ports use `csrrci mstatus, 8`), and a no-op in host builds — but on
silicon both ends failed at their first masked AES call (receiver: session
announces silently never transmit; keyboard: dead at announce-verify). The
mechanism was not isolated; the measurement stands. If hardening is revisited,
try `PFIC_DisableIRQ(BLEB_IRQn)` (IRQ 20; only `bb.o` imports `gptrAESReg`)
instead, and A/B it one end at a time with the wiring freshly verified.

**Traps already paid for — do not repeat:**
- Do NOT gate the driver on `AES_STA` bit 1 as completion evidence: the engine
  never sets it outside the vendor IRQ flow, so the gate rejects every healthy
  block and the link fails closed (session torn down per seal, reconnect
  churn).
- A "stale counter" incrementing when CFG completes with bit 1 clear counts
  every healthy block — it detects nothing.
- Poison-canary DATA writes cannot work (separate in/out latches); double
  compute is diagnostic, not a guarantee; do not reorder the STA/CFG handshake
  (matches the vendor's own `AES_DevAESEnc` exactly).

Validation once the fix lands: soak with the self-verify DISABLED (UART `B1 00`)
— the receiver must hold 0 MAC failures with no canary in place — then a soak
with it ENABLED, where `selfck_bad` should now stay ≈0 (post-TX, the verify
becomes a genuine detector again).

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

## 3. The open defect — RESOLVED, see §0

**~12% of sealed frames fail their tag**, with key, session, counter and frame
shape all provably correct. Nothing is lost in transit — the keyboard's sealed
count and the receiver's arrival count agree exactly.

The one strong clue, ~~unexplained~~ *(explained in §0: phase of the seal's
AES blocks relative to the radio-active window)*:

| condition | frames | verified | MAC failures | rate |
|---|---|---|---|---|
| idle (keepalives only) | 276 | 242 | 34 | **12.3%** |
| heavy HID traffic | 391 | 377 | 14 | **3.6%** |

More crypto activity produces *fewer* failures — because more activity shifts
which seal blocks overlap the TX window.

## 4. The experiment that was run (2026-08-15, outcome in §0)

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

*Outcome:* implemented (plus an `expect1`-vs-`expect2` determinism check, a
first-failure frame latch for the offline oracle, a FIPS KAT, and a
BB-interrupt-during-CCM correlator, all in one receiver flash). `mac_same_ok`
stayed 0, the receiver's tags were deterministic and oracle-identical, and the
latched frame convicted the keyboard per-frame — see §0.

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

Further traps paid for on 2026-08-15/16, same family:

- **minichlink CH5xx memory reads return PLAUSIBLE GARBAGE, not just zeros** —
  repeated reads of `.diag_safe` returned byte-identical stale junk across
  flashes and resets while the UART counters showed live values. A
  minichlink-GDB halt reads PC=0 on a *healthy* target too (verified against
  the working receiver as a control). Never diagnose from probe memory reads.
- **Opening a probe CDC port DTR-resets the target** (sometimes — the edge
  does not always fire), and after any reset **OpenBoot may hold the UART for
  ~10 s** before the app answers. Settle ≥11 s after opening a port, and hold
  ports open across a run.
- **The devboard UART wiring is marginal**: one deafness episode was cured
  only by physically reseating the PA8/PA9 jumpers, after firmware theories
  had consumed hours. Reseat first.

## 7. Other open items

### Carried from the 2026-08-15 audit (FINDINGS.md, retired 2026-08-16)

An independent audit reached the same AES/radio-overlap hypothesis that §0
proved, and proposed two of the experiments that proved it. Its still-open
findings, verified against source before the file was retired:

- **Capability negotiation is structurally broken, with a sharper mechanism
  than "the advert never lands":** the receiver latches the pre-beacon
  capability advert and then explicitly clears `rf_crypt_peer_capable` when it
  accepts the first fresh/unbonded beacon (OpenDongle `rf_task.c`, the
  peer-change accept path) — the pairing sequence erases the very latch it was
  meant to set. The advert carries no keyboard MAC, so the receiver cannot
  safely bind a pre-beacon advert to the identity that follows; the redesign
  must bind capability to the peer, not to arrival order. Also: the "one
  advert in four" comment does not match the code (first two slots, then one
  in eight — `pair_bcast_count & 0x07`).
- **Receiver `BondWrite` persists but does not activate.** IAP `BondWrite`
  calls only `bond_save()`: no key install, no `rf_crypt_bond_enc`, no fresh
  session. Runtime state is populated only at bond LOAD (boot). The
  provisioning tool prints `encryption ACTIVE` from persisted flags alone.
  Interim rule (institutionalized in `bench_run.py`): restart the receiver
  after provisioning. Proper fix: a key-state-changing BondWrite applies the
  live state atomically and forces a fresh session, or explicitly reboots.
- **Keyboard key persistence can falsely report success.**
  `rf_save_bond_to_flash()` does `(void)EEPROM_WRITE(...)` with no readback;
  `RF_ProvisionLinkKey()` then reports success. A failed save works from RAM
  until reboot and leaves the peers with different keys. Fix: return status,
  read back, validate, only then report.
- **Keyboard v1→v2 bond migration is absent by design** ("the version bump is
  deliberate and there is NO migration") while OpenDongle DOES migrate v1
  records — so a keyboard field-updated to an encrypted build drops its bond
  and needs a re-pair while the receiver keeps its half. Decide: mirror the
  dongle's migration, or document the forced re-pair.
- **Security-design gaps beyond key establishment** (see also the items
  below): session announces are authenticated but carry no freshness, so a
  recorded announce can be replayed to desynchronize the keyboard (DoS);
  session ids are 32-bit and generated, not reserved; the capability advert is
  unauthenticated; polls and LED relay (the downlink) are not authenticated at
  all — encryption today protects only the keyboard→dongle HID uplink.

Fixed out of the same audit (2026-08-16): the host-test `dongle_target.h`
include regression (both harnesses now compile against the CH592 target, which
also put the §0 diagnostics under host test — 137/137 pass); the
`KBD_RF_CRYPT=1 RF_DIAG_COUNTERS=0` link failure (the five `kbd_crypt_*`
counters and `systick_read` sat under the diag gate while crypto references
them unconditionally); and `make update` silently rebuilding plaintext (the
Makefile now refuses a defaulted-plaintext `update` while encrypted build
markers are present; explicit `KBD_RF_CRYPT=0` downgrades deliberately).

- **Receiver wedges.** The CH592 stopped answering IAP three times in one
  session, twice dropping off USB entirely; only a probe reset recovers it. This
  gates measurement — runs die partway. No credible mechanism found in the
  `DONGLE_RF_CRYPT` path; the bounded HAL and sparse traffic argue against
  crypto starvation. Needs its own investigation.
- **Capability advert never lands** -- FIXED 2026-08-22. The mechanism was not
  "the advert is lost" but "the advert arrives too late": the receiver commits
  the bond on the first beacon it hears, and the advert rode only slots 0,1 then
  one in eight, so a receiver joining mid-stream always met a beacon first.
  Measured 0/10 in the documented pairing order vs 11/11 with the dongle camped
  first; after leading every beacon with the advert, 12/12 in both.

  The earlier "beacon-chase" attempt DID break pairing entirely, and the reason
  is specific and worth keeping: it put the advert AFTER the beacon, inside the
  quiet window where the pair-ACK arrives, leaving the keyboard deaf
  (RF_Shut/RF_Tx) exactly then. The rule is therefore NOT "never touch the
  advert schedule" -- that would forbid the fix that worked -- but:
  **transmit nothing in the window immediately following a beacon.** Pinned as
  property P4 in firmware/tests/test_pair_slots.py.
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

**Pairing order is load-bearing FOR THE LINK, and no longer for capability.**
Select the transport (`A6 30`), put the KEYBOARD into pairing first, and only
then restart the dongle -- it accepts a pair only in the first few seconds after
boot, so its window must open while the keyboard is already broadcasting. The
same ordering applies to a bonded reconnect. Getting this backwards accounts for
every "the CH592 refuses to pair" observation in this project.

Until 2026-08-22 that same order was ALSO the one in which capability
negotiation failed: measured 0/10 pairs latched `ENC_CAPABLE` in this order
against 11/11 with the dongle camped first (Fisher p < 0.00001), because the
receiver commits on the first beacon it hears and the advert only rode one slot
in eight. The keyboard now leads every beacon with the advert, so capability
latches 12/12 in BOTH orders and the pairing order is once again only about
whether the link comes up.

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
`CEBD8F0653EF` (ttyACM1), receiver `CF148F065446` (ttyACM0). *(The
`C2228F064754` serial in older notes left the bench.)*

**The 2026-08 bench is UART-only** — neither target's USB is connected, so the
receiver's IAP/hidraw tooling is unreachable. What replaces it:

- Both CH592F devboards wire UART on the chip-default **PA8/PA9** (probe
  TX→PA8/RXD1, probe RX←PA9/TXD1): build the keyboard with
  `KBD_UART1_DEFAULT_PINS=1` (the PCB's PB12/PB13 remap stays the default and
  is silent on this bench), and flash with `make ... flash-factory
  KBD_PROBE=CEBD8F0653EF ALLOW_BONDED_FLASH=1`.
- The receiver (OpenDongle `em-ccm-bench-verify`) broadcasts its full crypto
  telemetry once per second on PA9 (`DONGLE_UART_DIAG`, 127-byte `0x5E`
  frame) and force-activates decryption for any valid loaded bond with the
  compiled-in bench key `4f70656e4b626421a55ac33c69960ff0`
  (`DONGLE_CRYPT_BENCH_FORCE_KEY`) — the keyboard gets the same key over
  `0xAE`. Both gates live in `ch592/src/dongle_target.h` and must never ship.
- Orchestration: `bench/bench_run.py [hold_s] --fresh` runs the whole
  sequence (fresh-state reset, pair, key, receiver power cycle into the
  encrypted epoch, hold with a link-keeper, reconciliation report);
  `bench/rx_uart_diag.py` follows the receiver telemetry alone.

**Fresh-pair recipe that always works** (mismatched bond states silently never
connect): wipe the receiver's bond (write 4 KiB of `0xFF` at DataFlash
`0x75000` over SDI, power-cycle → it camps in pairing indefinitely), keyboard
`A6 52` then `A6 51`, key with `0xAE` *after* pairing, then power-cycle the
receiver and immediately re-send `A6 30` so the keyboard broadcasts inside the
receiver's ~3 s boot window (mint + announce happen at that connect).

## 9. References

- `firmware/docs/LINK-ENCRYPTION-STATUS.md` — feature status and design notes
- `firmware/docs/CRASH-DEBUG-PLAN.md` (branch `firmware-crash-debug`) — the
  refuted HardFault investigation, retained as a record; **its premise is void**
- Codex sessions: `019feb15-3314-7e71-a94e-bc9453eabea6` (first AES review),
  `019febc6-c8a4-78b2-b5c0-bca2072f95fb` (adversarial review of the crash plan),
  `019fec05-2531-7ae3-9869-139bf780fd68` (disconnect chain arithmetic),
  `019fec41-5126-7603-bf11-17a7ad62f579` (AES asymmetry + the §4 experiment)
