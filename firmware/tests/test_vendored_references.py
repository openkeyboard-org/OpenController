"""Grade the vendored references against their external ground truth.

`tests/README.md` describes a trust chain: `aes_vectors.py` carries FIPS-197 ECB
and RFC 3610 CCM vectors as "external ground truth", and `ccm_ref.py` is "the
authoritative host reference for the wire format, graded against RFC 3610".

Nothing in this repository actually exercised that. The vectors sat unused --
the only tests here were `test_kbd_rf_crypt.py` and `test_pair_slots.py`,
neither of which imports `aes_vectors` -- so the README asserted a trust chain
the suite never checked. These files are COPIES that the README itself warns can
go stale when the receiver's wire format moves; an unexercised reference is
exactly how that staleness stays invisible.

This closes the loop cheaply: if a re-copy ever brings across a `ccm_ref.py`
that disagrees with RFC 3610, or an `aes_vectors.py` whose expected outputs were
mangled, these fail immediately rather than silently grading the keyboard
against a format nothing speaks.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import aes_vectors  # noqa: E402
import ccm_ref  # noqa: E402


class VendoredAesMatchesFips197(unittest.TestCase):
    def test_ecb_vectors(self):
        self.assertTrue(aes_vectors.VECTORS, "no AES vectors vendored")
        for name, key, plain, expect in aes_vectors.VECTORS:
            with self.subTest(vector=name):
                got = ccm_ref.aes128_encrypt_block(
                    bytes.fromhex(key), bytes.fromhex(plain))
                self.assertEqual(got.hex(), expect.lower(),
                                 f"{name}: ccm_ref AES disagrees with the vector")


class VendoredCcmMatchesRfc3610(unittest.TestCase):
    def test_ccm_packet_vectors(self):
        self.assertTrue(aes_vectors.CCM_VECTORS, "no CCM vectors vendored")
        for v in aes_vectors.CCM_VECTORS:
            with self.subTest(vector=v["name"]):
                got = ccm_ref.ccm_encrypt(
                    bytes.fromhex(v["key"]),
                    bytes.fromhex(v["nonce"]),
                    bytes.fromhex(v["aad"]),
                    bytes.fromhex(v["payload"]),
                )
                self.assertEqual(got.hex(), v["ct_tag"].lower(),
                                 f"{v['name']}: ccm_ref CCM disagrees with RFC 3610")

    def test_the_vectors_use_the_wire_parameters(self):
        """M=8 (8-byte tag) and L=2 (13-byte nonce) -- the link's parameters.

        A vector set silently re-copied at different CCM parameters would still
        pass the round trip above while proving nothing about this wire format.
        """
        for v in aes_vectors.CCM_VECTORS:
            with self.subTest(vector=v["name"]):
                nonce = bytes.fromhex(v["nonce"])
                payload = bytes.fromhex(v["payload"])
                ct_tag = bytes.fromhex(v["ct_tag"])
                self.assertEqual(len(nonce), 13, "L=2 requires a 13-byte nonce")
                self.assertEqual(len(ct_tag) - len(payload), 8,
                                 "M=8 requires an 8-byte tag")


if __name__ == "__main__":
    unittest.main()
