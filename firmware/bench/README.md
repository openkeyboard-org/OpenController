# Link-encryption bench diagnostics

Host scripts used to characterise the encrypted link on hardware. They talk to
the CH592 receiver over its vendor HID (IAP) interface and to the keyboard over
SWD via `minichlink`. Neither resets the target.

Requires `pyserial`. Probe serials and the `minichlink` path are constants at the
top of each script — edit them for a different bench.

## `crypt_diag.py`

One-shot read of the receiver's `CMD_CRYPT_DIAG` (IAP `0x94`) plus its bond
record. Prints the verified count, the per-reason drop tally, and the pre-verify
sink counters.

The pre-verify counters are the point. `ok` and the reason array only count
frames that already reached `rf_crypt_rx()`; a frame lost before that leaves them
all at zero, which reads identically to "the keyboard sent nothing". `conn_rx`,
`len_max`, `enc_shape`, `fifo_full`, `flush_drop` and `plain_drop` split those
cases apart. `len_max` is the decisive one: it is recorded *before* any
classification, so a frame the classifier rejects still leaves evidence of what
actually arrived on air.

Two protocol details that cost time to rediscover:

- IAP packets are `[report-id 0][cmd][len][body][checksum]` padded to 65 bytes,
  `checksum = (cmd + len + sum(body)) & 0xFF`. A packet without the checksum is
  ignored **silently** — the symptom is a bare timeout, not an error.
- `BondRead` replies `[ack][len][status][record…]`, so the record starts at
  offset **3**. Reading from offset 2 shifts every field by one byte and yields a
  plausible-looking but wrong version/flags. The link key is redacted by design.

## `both_ends.py`

Runs one connected session and reconciles the keyboard's transmit counters
against the receiver's receive counters:

    sealed  vs  enc_shape   -> did every sealed frame reach the sink?
    enc_shape vs ok + mac   -> did every arriving frame verify?

A shortfall in the first is a transmit or air-loss problem; a shortfall in the
second is a crypto-state problem. They point at completely different code.

Caveat: the keyboard's `.diag_safe` counters are zeroed by `RF_TaskInit`, which
runs once per boot. If the keyboard reboots mid-run the deltas are meaningless
and can even go negative — check `watch_reboot.py` before trusting a result.

## `watch_reboot.py`

Answers "is the keyboard rebooting?" without needing any new symbol.
`kbd_crypt_tx_sealed` is zeroed only by `RF_TaskInit`, so it otherwise increases
monotonically — a **decrease** between two samples is a reboot, full stop.

`ll_boot_count` is the direct version of the same signal: `RF_TaskInit` is called
exactly once from `main.c`, so every increment is a real reboot rather than a
re-init.

## Addresses shift

Every `.diag_safe` address moves whenever a counter is added or removed. Always
re-derive them from the current `build/ch592f-slotA/opencontroller_ch592f.map`
rather than reusing a value from notes. Related: `rf_last_tx_status`,
`rf_last_rx_status` and `rf_last_config_status` are packed `uint8_t`, not words —
a 4-byte read blends three fields into one plausible-looking number.
