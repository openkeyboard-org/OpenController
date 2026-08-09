# Host tests

Run from the repository root (no hardware, no cross-compiler needed — these
build for the host):

```bash
cd firmware/tests && python3 -m unittest discover
```

## What is tested

`test_kbd_rf_crypt.py` compiles the real `src/kbd_rf_crypt.c` against the
portable software cipher — through a `hal_aes` shim standing in for the CH592
hardware engine — and grades every frame it emits against `ccm_ref.py`.

This is the gate that matters most before bench time. The keyboard encrypts and
the receiver decrypts, so the two must agree byte-for-byte; if they do not, the
link pairs and then silently drops every frame on the MAC check, which is a
miserable thing to diagnose on hardware. Graded here instead.

Beyond format agreement the suite pins the fail-closed behaviours: a wedged AES
engine never emits a frame, a spent counter is never reused, sealing without a
session is refused, and a session frame with any tampered byte — including the
cleartext `session_id`, which is bound only through the nonce — fails the MAC.

## Vendored references

These files are **copies**, not originals. They come from OpenDongle (same
authorship, also Apache-2.0), branch `em-rf-ccm-rx-decrypt` at
`5713869f01acc582bab8b4b20023ca5bf2517235`:

| File | Origin in OpenDongle | Why it is here |
|---|---|---|
| `ccm_ref.py` | `firmware/tests/ccm_ref.py` | The authoritative host reference for the wire format, graded against RFC 3610. `build_frame()` emits exactly the bytes a conforming keyboard must transmit. |
| `aes_vectors.py` | `firmware/tests/aes_vectors.py` | FIPS-197 ECB and RFC 3610 CCM vectors — external ground truth. |
| `aes_sw.c`, `aes_sw.h` | `firmware/common/{src,include}/aes_sw.*` | Portable AES-128 so the tests build with no hardware. **Test scaffolding only** — never the firmware's runtime path, which uses the CH592 hardware engine via `src/hal_aes_ch592.c` (~35x faster). |

They are copied rather than referenced across repositories so the suite runs on
a lone checkout. That has a cost: if the receiver's wire format ever moves,
these copies go stale and the tests would happily grade the keyboard against a
format nothing speaks. Re-copy them whenever the OpenDongle crypt branch moves,
and treat a disagreement as the receiver being right.

The pinned end-to-end vector in `test_pinned_on_silicon_vector` is the one
assertion immune to that drift: it is a fixed byte string taken from
OpenDongle's on-silicon suite (`firmware/validation/aes_validate.c`), so it
fails if either side changes, whatever the vendored references say.
