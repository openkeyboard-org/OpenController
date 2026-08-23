# Link encryption (AES-128-CCM) — status

Branch: `firmware-link-encryption` (un-parked 2026-08-16).
Companion receiver work: OpenDongle branch `em-ccm-bench-verify`.

**2026-08-16: the blocking defect is root-caused and the encrypted link runs
clean** — 0 MAC failures over 33.5k- and 20.8k-keepalive idle soaks (vs the
historical 12.3%). The CH592 AES engine silently returns the previous block's
output when a block operation is preempted by the BLEB radio interrupt; the
keyboard's seal overlapped its own transmission. Full mechanism, evidence
chain, and the prescribed structural fix: `docs/TODO.md` §0. (The earlier
"keyboard HardFaults on a timer" parking rationale was itself refuted as a
probe artifact — TODO.md §6.)

## What is done and proven

The keyboard transmit path is complete and **verified on hardware end to end**.
A CH592 receiver running the `em-ccm-bench-verify` build, which forwards nothing
to USB without a passing CCM tag, delivered keystrokes from this firmware. That
delivery is the proof: it cannot happen unless the tag verified.

Receiver-side telemetry over one connected run (IAP `CMD_CRYPT_DIAG`):

| counter | reading | meaning |
|---|---|---|
| `len_max` / tag | 22 / `0xA1` | sealed frames arrive intact and correctly shaped |
| `enc_shape` | 52 – 562 | the classifier accepted them |
| **`ok` (verified)** | **45 – 516** | the CCM tag passed |
| `fifo_full`, `flush_drop` | 0, 0 | nothing lost between arrival and verification |

Keepalive cadence measured at 1 sealed frame per ~31–32 receptions, matching
`KBD_CRYPT_KEEPALIVE_POLLS = 32` exactly.

Also complete: 32 host tests against OpenDongle's `ccm_ref.py` golden corpus plus
the pinned on-silicon vector; the `KBD_RF_CRYPT=0` build stays **byte-identical**
to the validated plaintext image (`sha256 ccdfa519…b47e4b2`), so the feature gate
is genuinely zero-impact.

## What is not done

- **Phase 2 (ECDH key establishment) is not started.** The link key is still a
  bench scaffold written over UART behind `KBD_CRYPT_BENCH_KEY`, which must never
  ship enabled. See the plan for the X25519 design.
- **The capability advert never lands** — every pair records `capable=no`, and
  `bond_enc_active()` needs `capable AND key`. Bench work uses
  `provision_link_key.py`, which sets both flags. A beacon-chase fix was tried and
  **broke pairing entirely** (the keyboard goes deaf transmitting the advert
  exactly when the ACK arrives); do not repeat it. This needs protocol-level
  design, not a scheduling tweak.
- **CCM counter persistence across reboot.** The per-boot random start
  (`4ef00be`) makes keystream reuse unlikely but does not guarantee it. Reserved
  counter ranges in flash, or per-session keys, is the real answer.
- One remaining Codex finding: the ISR race on `seal_*`, only reachable under
  `STOCK_ISR_FAST_RESPONSE=1`, which is not the default.

  The other two from that set are **closed**, and this list used to keep
  reporting them as open — operators were being pointed at finished work:
  - *Seal timing* — landed in two halves (arm from TX_FINISH; `seal_begin`
    derives every block twice and requires agreement) and validated
    2026-08-16 over 300 s idle soaks, 0 MAC failures with `seal_redo` catching
    742 / 2273 live collisions. See `TODO.md` section 0.
  - *`rf_clear_bond_flash` discarding the `EEPROM_ERASE` result* — now checks
    the erase and re-reads to confirm, and `RF_ClearBond()` reports failure so
    `A6 52` answers `0x36` instead of falsely acking.

## The measurement that reframed everything

Four rounds of investigation concluded "the receiver never verifies our frames".
That was **wrong**, and wrong in an instructive way: every counter then exported
by `CMD_CRYPT_DIAG` counted only frames that had already reached `rf_crypt_rx()`.
A frame lost before that point left every counter at zero — indistinguishable
from "the keyboard transmitted nothing".

Adding pre-verify counters on the receiver (`conn_rx`, `len_max`, `enc_shape`,
`fifo_full`, `flush_drop`, `plain_drop`) showed the data path had been working the
whole time. The real fault was that the **transmitter kept rebooting**, so every
earlier reading was taken against a keyboard that restarted mid-session.

The lesson worth keeping: a counter that only increments after the stage you
suspect cannot exonerate the stages before it.

## Reproducing the bench setup

Receiver (CH592, probe `C2228F064754`) — app GCC15, bootloader GCC12:

```sh
cd firmware/ch592     # in OpenDongle
make MRS_TOOLCHAIN=".../GCC15/bin" OPENBOOT_TOOLCHAIN=".../GCC12/bin" bundle
```

Update over OBP rather than `flash-factory` — **the bond survives an OBP update**
and a factory flash erases it. Two transients must be retried through, not raced:
the device re-enumerates on the way into the bootloader, and udev takes a moment
to apply the plugdev ACL to the fresh node. Retry on both "no HID device" and
"Permission denied"; once any command completes a HELLO the bootloader is
fail-stay.

Keyboard (probe `CEBD8F0653EF`):

```sh
make KBD_RF_CRYPT=1 KBD_CRYPT_BENCH_KEY=1 bundle
make update OB_PORT=/dev/serial/by-id/usb-wch.cn_WCH-Link_CEBD8F0653EF-if01
```

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

## Diagnostics

Receiver: `CMD_CRYPT_DIAG` (IAP `0x94`, served unarmed, 50-byte payload). IAP
packets are `[report-id 0][cmd][len][body][checksum]` padded to 65 bytes, where
`checksum = (cmd + len + sum(body)) & 0xFF` — an unchecksummed packet is ignored
silently and simply times out. `BondRead` (`0x88`) needs the dispatcher armed
(handshake `0x5A "WCH@HFD"`, then `GetDevInfo(1)`); its reply is
`[ack][len][status][record…]`, so the record starts at offset **3**, and the link
key is deliberately redacted.

Keyboard `.diag_safe` counters are read over the keyboard UART (0xAF counters, 0xB0 fail latch). Do NOT read them over SWD: a probe attach resets a running application and CH5xx probe memory reads return plausible stale garbage -- see TODO.md section 6, which is why those counters were moved to UART. Addresses shift whenever a counter is added — always
re-derive them from the current `.map`. `rf_last_tx_status` / `rx_status` /
`config_status` are packed `uint8_t`, not words; read the region and decode
against the map rather than reading single symbols.

`ll_boot_count` is the reboot detector: `RF_TaskInit` is called exactly once from
`main.c`, so every increment is a real reboot. `kbd_crypt_tx_sealed` going
*backwards* between two samples is the same signal, cheaply.

## Remaining work (2026-08-16)

1. Land the structural fix (TODO.md §0): arm the seal from TX_FINISH + mask
   `mstatus.MIE` per AES block; validate with the self-verify disabled, then
   enabled.
2. Negative tests on the healthy link (plaintext frame on an encrypted bond
   must be dropped; a replayed frame must be rejected), and an end-to-end HID
   check once a receiver with USB is back on the bench.
3. Then the capability advert, then counter persistence, then Phase 2 (ECDH).
4. Strip the bench scaffolding before anything ships: `KBD_CRYPT_BENCH_KEY`,
   `DONGLE_CRYPT_BENCH_FORCE_KEY`, `DONGLE_UART_DIAG`, and the compiled bench
   key.
