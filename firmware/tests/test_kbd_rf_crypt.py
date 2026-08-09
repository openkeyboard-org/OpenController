"""The keyboard's AES-128-CCM TX path must produce exactly what the receiver decrypts.

Compiles the real `src/kbd_rf_crypt.c` against the portable software cipher
(through a tiny hal_aes shim, standing in for the CH592 hardware engine) and
grades every frame it emits against `ccm_ref`, which is anchored to RFC 3610 and
is the same reference the receiver's own tests are graded against. If these two
disagree the link pairs and then drops every frame on the MAC check, so this is
the gate that matters most before any bench time.

Also pins the one end-to-end frame that OpenDongle's on-silicon suite asserts
(`firmware/validation/aes_validate.c`), which is a fixed byte string and cannot
drift with either implementation.

Run: python3 -m unittest discover -s firmware/tests
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import ccm_ref

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"
CRYPT_C = SRC / "kbd_rf_crypt.c"
AES_SW_C = HERE / "aes_sw.c"

TAG_BOOT, TAG_CONSUMER, TAG_MOUSE = 0xA1, 0xA3, 0xA8
TAG_SESSION = 0xA5
BODY_LEN = {TAG_BOOT: 8, TAG_CONSUMER: 2, TAG_MOUSE: 5}

# hal_aes shim over the software cipher, plus a line-driven harness so session
# and counter state persist across operations within one process.
HARNESS = r"""
#include "kbd_rf_crypt.h"
#include "hal_aes.h"
#include "aes_sw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static aes_sw_ctx_t g_ctx;
static int g_fail_next = 0;

void hal_aes_init(void) {}
void hal_aes_set_key(const uint8_t key[16]) { aes_sw_expand_key(&g_ctx, key); }
hal_aes_status_t hal_aes_encrypt_block(const uint8_t in[16], uint8_t out[16]) {
    if (g_fail_next) { g_fail_next = 0; memset(out, 0, 16); return HAL_AES_ENGINE_TIMEOUT; }
    aes_sw_encrypt_block(&g_ctx, in, out);
    return HAL_AES_OK;
}

static int hexval(char c){
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return -1;
}
static int unhex(const char *s, uint8_t *out, int max){
    int n=0;
    if (!s) return 0;
    while (s[0]&&s[1]&&hexval(s[0])>=0&&hexval(s[1])>=0&&n<max){
        out[n++]=(uint8_t)((hexval(s[0])<<4)|hexval(s[1])); s+=2;
    }
    return n;
}
static void puthex(const uint8_t *b, int n){
    for (int i=0;i<n;i++) printf("%02x", b[i]);
}
static const char *st_name(kbd_crypt_status_t s){ switch(s){
    case KBD_CRYPT_OK: return "OK";
    case KBD_CRYPT_INACTIVE: return "INACTIVE";
    case KBD_CRYPT_SHAPE: return "SHAPE";
    case KBD_CRYPT_MAC: return "MAC";
    case KBD_CRYPT_EXHAUSTED: return "EXHAUSTED";
    case KBD_CRYPT_BUSY: return "BUSY";
    case KBD_CRYPT_FAULT_ENGINE: return "ENGINE";
    default: return "?"; } }

int main(void){
    char line[512];
    uint8_t buf[64], frame[64];
    uint8_t out_len;
    while (fgets(line, sizeof(line), stdin)) {
        char *cmd = strtok(line, " \t\r\n");
        if (!cmd) continue;
        char *a1 = strtok(NULL, " \t\r\n");
        char *a2 = strtok(NULL, " \t\r\n");
        char *a3 = strtok(NULL, " \t\r\n");

        if (!strcmp(cmd, "install")) {
            unhex(a1, buf, 16); kbd_crypt_install_key(buf); printf("OK\n");
        } else if (!strcmp(cmd, "session")) {
            unsigned long v = strtoul(a1, NULL, 16);
            kbd_crypt_adopt_session((uint32_t)v); printf("OK\n");
        } else if (!strcmp(cmd, "endsession")) {
            kbd_crypt_end_session(); printf("OK\n");
        } else if (!strcmp(cmd, "clear")) {
            kbd_crypt_clear(); printf("OK\n");
        } else if (!strcmp(cmd, "active")) {
            printf("%d\n", kbd_crypt_active());
        } else if (!strcmp(cmd, "failnext")) {
            g_fail_next = 1; printf("OK\n");
        } else if (!strcmp(cmd, "seal")) {
            unsigned long ctrl = strtoul(a1, NULL, 16);
            unsigned long tag  = strtoul(a2, NULL, 16);
            int n = unhex(a3, buf, sizeof(buf));
            kbd_crypt_status_t s = kbd_crypt_seal((uint8_t)ctrl, (uint8_t)tag,
                                                  buf, (uint8_t)n, frame, &out_len);
            if (s == KBD_CRYPT_OK) { printf("OK "); puthex(frame, out_len); printf("\n"); }
            else printf("%s\n", st_name(s));
        } else if (!strcmp(cmd, "begin")) {
            unsigned long tag = strtoul(a1, NULL, 16);
            int n = unhex(a2, buf, sizeof(buf));
            printf("%s\n", st_name(kbd_crypt_seal_begin((uint8_t)tag, buf, (uint8_t)n)));
        } else if (!strcmp(cmd, "finish")) {
            unsigned long ctrl = strtoul(a1, NULL, 16);
            kbd_crypt_status_t s = kbd_crypt_seal_finish((uint8_t)ctrl, frame, &out_len);
            if (s == KBD_CRYPT_OK) { printf("OK "); puthex(frame, out_len); printf("\n"); }
            else printf("%s\n", st_name(s));
        } else if (!strcmp(cmd, "verify")) {
            int n = unhex(a1, frame, sizeof(frame));
            uint32_t sid = 0;
            kbd_crypt_status_t s = kbd_crypt_verify_session(frame, (uint8_t)n, &sid);
            if (s == KBD_CRYPT_OK) printf("OK %08x\n", sid);
            else printf("%s\n", st_name(s));
        } else if (!strcmp(cmd, "claim")) {
            printf("%d\n", kbd_crypt_try_claim());
        } else if (!strcmp(cmd, "release")) {
            kbd_crypt_release(); printf("OK\n");
        } else {
            printf("?\n");
        }
        fflush(stdout);
    }
    return 0;
}
"""

KEY = bytes(range(1, 17))
SID = 0x11223344


def _cc():
    for c in ("cc", "gcc", "clang"):
        if shutil.which(c):
            return c
    raise unittest.SkipTest("no host C compiler")


class Harness:
    def __init__(self, tmp: Path):
        self.bin = tmp / "harness"
        (tmp / "harness.c").write_text(HARNESS)
        subprocess.run(
            [_cc(), "-O1", "-std=gnu99", "-Wall", "-Wextra", "-Wno-unused-parameter",
             f"-I{SRC}", f"-I{HERE}", "-o", str(self.bin),
             str(tmp / "harness.c"), str(CRYPT_C), str(AES_SW_C)],
            check=True, capture_output=True,
        )
        self.p = subprocess.Popen([str(self.bin)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)

    def cmd(self, line: str) -> str:
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=10)
        self.p.stdout.close()


class KbdCryptTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.h = Harness(Path(self._tmp.name))
        self.h.cmd(f"install {KEY.hex()}")
        self.h.cmd(f"session {SID:08x}")

    def tearDown(self):
        self.h.close()
        self._tmp.cleanup()

    # ---------------------------------------------------------- wire format

    def test_frames_match_reference_every_shape(self):
        """Each report shape seals to exactly the reference bytes."""
        for tag, n in BODY_LEN.items():
            with self.subTest(tag=hex(tag)):
                self.h.cmd(f"session {SID:08x}")     # counter restarts at 1
                body = bytes(range(0x10, 0x10 + n))
                ctrl = 0x5A
                got = self.h.cmd(f"seal {ctrl:02x} {tag:02x} {body.hex()}")
                want = ccm_ref.build_frame(KEY, SID, 1, ctrl, tag, body)
                self.assertEqual(got, "OK " + want.hex())

    def test_pinned_on_silicon_vector(self):
        """The exact frame OpenDongle's on-silicon suite asserts. Cannot drift."""
        self.h.cmd(f"session {SID:08x}")
        got = self.h.cmd(f"seal 5a a3 {bytes([0xE9, 0x00]).hex()}")
        self.assertEqual(got, "OK 5aa301000000f59261bf78f0e660f668")

    def test_counter_increments_per_transmission(self):
        """Consecutive seals of the SAME report must use different counters.

        This is the nonce-reuse guard: the link retransmits an unchanged report
        several times, each possibly under a different ctrl (which is AAD).
        """
        body = bytes(8)
        first = self.h.cmd(f"seal 02 a1 {body.hex()}")
        second = self.h.cmd(f"seal 02 a1 {body.hex()}")
        self.assertNotEqual(first, second)
        self.assertEqual(first, "OK " + ccm_ref.build_frame(KEY, SID, 1, 0x02, TAG_BOOT, body).hex())
        self.assertEqual(second, "OK " + ccm_ref.build_frame(KEY, SID, 2, 0x02, TAG_BOOT, body).hex())

    def test_counter_starts_at_one_and_never_zero(self):
        """Counter 0 is reserved; the receiver rejects it as a replay."""
        got = self.h.cmd(f"seal 02 a1 {bytes(8).hex()}")
        self.assertTrue(got.startswith("OK "))
        self.assertEqual(bytes.fromhex(got[3:])[2:6], (1).to_bytes(4, "little"))

    def test_split_seal_matches_one_shot(self):
        """begin()+finish() must produce the byte-identical frame to seal()."""
        body = bytes(range(0x20, 0x28))
        self.h.cmd(f"session {SID:08x}")
        one_shot = self.h.cmd(f"seal 03 a1 {body.hex()}")
        self.h.cmd(f"session {SID:08x}")
        self.assertEqual(self.h.cmd(f"begin a1 {body.hex()}"), "OK")
        split = self.h.cmd("finish 03")
        self.assertEqual(split, one_shot)

    def test_ctrl_is_authenticated(self):
        """ctrl is AAD: the same payload under a different ctrl differs in MAC."""
        body = bytes(8)
        self.h.cmd(f"session {SID:08x}")
        self.assertEqual(self.h.cmd(f"begin a1 {body.hex()}"), "OK")
        a = bytes.fromhex(self.h.cmd("finish 00")[3:])
        self.h.cmd(f"session {SID:08x}")
        self.assertEqual(self.h.cmd(f"begin a1 {body.hex()}"), "OK")
        b = bytes.fromhex(self.h.cmd("finish 01")[3:])
        self.assertNotEqual(a[-8:], b[-8:])           # MAC differs
        self.assertEqual(a[6:14], b[6:14])            # ciphertext identical

    def test_every_ctrl_value_verifies(self):
        """ctrl is seeded 0x02 and only bits 0-1 move, so all 4 must work."""
        body = bytes(range(8))
        for ctrl in (0x00, 0x01, 0x02, 0x03):
            with self.subTest(ctrl=ctrl):
                self.h.cmd(f"session {SID:08x}")
                got = self.h.cmd(f"seal {ctrl:02x} a1 {body.hex()}")
                want = ccm_ref.build_frame(KEY, SID, 1, ctrl, TAG_BOOT, body)
                self.assertEqual(got, "OK " + want.hex())

    def test_receiver_can_open_what_we_seal(self):
        """Round-trip through the reference decryptor, the receiver's own path."""
        body = bytes([0x00, 0x00, 0x04, 0, 0, 0, 0, 0])   # 'a' pressed
        got = bytes.fromhex(self.h.cmd(f"seal 02 a1 {body.hex()}")[3:])
        opened = ccm_ref.open_frame(KEY, SID, got)
        self.assertIsNotNone(opened)
        counter, ctrl, tag, plain = opened
        self.assertEqual((counter, ctrl, tag, plain), (1, 0x02, TAG_BOOT, body))

    # ------------------------------------------------------------- sessions

    def test_session_frame_verifies_and_reports_id(self):
        frame = self._session_frame(SID, ctrl=0x40)
        self.assertEqual(self.h.cmd(f"verify {frame.hex()}"), f"OK {SID:08x}")

    def test_session_frame_rejects_tampered_id(self):
        """session_id is cleartext and NOT in the AAD -- the nonce binds it."""
        frame = bytearray(self._session_frame(SID, ctrl=0x40))
        frame[2] ^= 0x01
        self.assertEqual(self.h.cmd(f"verify {bytes(frame).hex()}"), "MAC")

    def test_session_frame_rejects_tampered_ctrl_and_mic(self):
        base = self._session_frame(SID, ctrl=0x40)
        for idx in (0, 6, 13):
            with self.subTest(byte=idx):
                frame = bytearray(base)
                frame[idx] ^= 0x01
                self.assertEqual(self.h.cmd(f"verify {bytes(frame).hex()}"), "MAC")

    def test_session_frame_rejects_wrong_shape(self):
        base = self._session_frame(SID, ctrl=0x40)
        self.assertEqual(self.h.cmd(f"verify {base[:-1].hex()}"), "SHAPE")
        wrong_tag = bytearray(base)
        wrong_tag[1] = 0xA1
        self.assertEqual(self.h.cmd(f"verify {bytes(wrong_tag).hex()}"), "SHAPE")

    def test_adopting_session_restarts_counter(self):
        body = bytes(8)
        self.h.cmd(f"seal 02 a1 {body.hex()}")
        self.h.cmd(f"seal 02 a1 {body.hex()}")
        self.h.cmd(f"session {SID:08x}")              # re-announce, same id
        got = self.h.cmd(f"seal 02 a1 {body.hex()}")
        self.assertEqual(got, "OK " + ccm_ref.build_frame(KEY, SID, 1, 0x02, TAG_BOOT, body).hex())

    def test_new_session_id_changes_ciphertext(self):
        body = bytes(8)
        self.h.cmd(f"session {SID:08x}")
        a = self.h.cmd(f"seal 02 a1 {body.hex()}")
        self.h.cmd(f"session {SID ^ 1:08x}")
        b = self.h.cmd(f"seal 02 a1 {body.hex()}")
        self.assertNotEqual(a, b)

    # --------------------------------------------------------- fail-closed

    def test_seal_inactive_without_session(self):
        self.h.cmd("endsession")
        self.assertEqual(self.h.cmd(f"seal 02 a1 {bytes(8).hex()}"), "INACTIVE")

    def test_seal_inactive_after_clear(self):
        self.h.cmd("clear")
        self.assertEqual(self.h.cmd(f"seal 02 a1 {bytes(8).hex()}"), "INACTIVE")
        self.assertEqual(self.h.cmd("active"), "0")

    def test_verify_inactive_without_key(self):
        self.h.cmd("clear")
        frame = self._session_frame(SID, ctrl=0x40)
        self.assertEqual(self.h.cmd(f"verify {frame.hex()}"), "INACTIVE")

    def test_bad_body_length_refused(self):
        self.assertEqual(self.h.cmd(f"seal 02 a1 {bytes(7).hex()}"), "SHAPE")
        self.assertEqual(self.h.cmd(f"seal 02 a3 {bytes(8).hex()}"), "SHAPE")
        self.assertEqual(self.h.cmd(f"seal 02 ff {bytes(8).hex()}"), "SHAPE")

    def test_finish_without_begin_refused(self):
        """A second finish() must not re-emit under an already-spent counter."""
        self.h.cmd(f"begin a1 {bytes(8).hex()}")
        self.assertTrue(self.h.cmd("finish 02").startswith("OK "))
        self.assertEqual(self.h.cmd("finish 02"), "SHAPE")

    def test_engine_fault_never_emits_a_frame(self):
        """A wedged engine must fail loudly, never transmit."""
        self.h.cmd("failnext")
        self.assertEqual(self.h.cmd(f"seal 02 a1 {bytes(8).hex()}"), "ENGINE")

    def test_engine_fault_in_finish_never_emits(self):
        self.assertEqual(self.h.cmd(f"begin a1 {bytes(8).hex()}"), "OK")
        self.h.cmd("failnext")
        self.assertEqual(self.h.cmd("finish 02"), "ENGINE")

    def test_engine_claim_is_exclusive(self):
        self.assertEqual(self.h.cmd("claim"), "1")
        self.assertEqual(self.h.cmd("claim"), "0")
        self.h.cmd("release")
        self.assertEqual(self.h.cmd("claim"), "1")

    # ------------------------------------------------------------- helpers

    def _session_frame(self, sid: int, ctrl: int) -> bytes:
        """Build the dongle->keyboard session frame the way the receiver does:
        empty payload, AAD {ctrl, 0xA5}, direction 0x02, counter 0."""
        nonce = ccm_ref.build_nonce(sid, ccm_ref.DIR_DONGLE_TO_KB, 0)
        aad = bytes([ctrl, TAG_SESSION])
        mic = ccm_ref.ccm_encrypt(KEY, nonce, aad, b"", ccm_ref.CCM_TAG_BYTES)
        return bytes([ctrl, TAG_SESSION]) + sid.to_bytes(4, "little") + mic


if __name__ == "__main__":
    unittest.main()
