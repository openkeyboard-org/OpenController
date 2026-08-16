# Link-encryption bench diagnostics

Host scripts for characterising the encrypted link on the UART-only bench (no
target USB): keyboard on WCH-Link `CEBD8F0653EF` (ttyACM1), receiver on
`CF148F065446` (ttyACM0), both CH592F devboards with UART on the chip-default
PA8/PA9. Requires `pyserial`.

Two rules the scripts embody, learned the hard way (`docs/TODO.md` §6):

- **Never read the keyboard over SWD during a run.** A `minichlink` attach
  resets the target, and CH5xx probe memory reads return plausible stale
  garbage even when they "work". Counters travel over UART only: keyboard
  `0xAF`/`0xB0`, receiver 1 Hz `0x5E` telemetry broadcast.
- **Opening a probe CDC port can DTR-reset its target**, and after a reset
  OpenBoot may hold the UART for ~10 s. Open each port once, settle ≥11 s,
  and hold the handle for the whole run.

## `bench_run.py [hold_seconds] [--fresh]`

The orchestrator: pairs (keyboard `A6 51` first — the receiver only accepts in
the first seconds after boot), keys both ends (`0xAE` + the bench key baked
into the receiver's `DONGLE_CRYPT_BENCH_FORCE_KEY`), power-cycles the receiver
into the encrypted epoch, holds with a link-keeper, and prints the
keyboard-vs-receiver reconciliation plus the TODO.md §4 decision numbers.

`--fresh` first wipes the receiver's DataFlash bond over SDI and clears the
keyboard's with `A6 52` — mismatched bond states silently never connect, so
start every characterisation run fresh.

## `rx_uart_diag.py [--follow] [--seconds N]`

Follows the receiver's `0x5E` telemetry (all `CMD_CRYPT_DIAG` counters plus
`mac_same_ok`, `same_differs`, `bb_during_aes`, the KAT result, and the
first-failure frame latch for the offline `ccm_ref.py` oracle). One-shot by
default; `--follow` prints one line per frame with deltas.

## `crypt_diag.py` / `both_ends.py` / `watch_reboot.py`

The USB-era tools, kept for when a receiver with USB returns to the bench.
`crypt_diag.py` reads `CMD_CRYPT_DIAG` over hidraw (needs the dongle's own
USB). `both_ends.py` still reads the keyboard over SWD — port it to `0xAF`
before trusting it. **Do not run `watch_reboot.py`**: its polling loop is the
probe-attach reboot artifact in executable form (TODO.md §6), and its
docstring's claim that SWD reads don't reset the target is refuted.

## Protocol notes that cost time to rediscover

- Keyboard UART frames are `[cmd][body...][chk]`, `chk = sum & 0xFF`. `0xAF`
  → `[0x5D][9 × u32 LE][chk]` (seal_miss, tx_sealed, sess_bad, sess_ok,
  sess_rx, selfck_ok, selfck_bad, bb_during_aes, spare). `0xB0` → the
  self-verify failure latch (frame + recomputed tag + keystream blocks).
  `[0xB1][mode]` toggles the pre-seal self-verify at runtime.
- The self-verify counters are a hazard-window canary, not a link-health
  metric — while the verify overlaps the radio-active window its own AES gets
  corrupted and `selfck_bad` reads ~100% even when every frame verifies on
  air. The receiver's `ok`/`drop` counters are the ground truth.
- IAP packets (USB era) are `[report-id 0][cmd][len][body][checksum]` padded
  to 65 bytes; an unchecksummed packet is ignored silently. `BondRead`'s
  record starts at offset **3**; the link key is redacted by design.
