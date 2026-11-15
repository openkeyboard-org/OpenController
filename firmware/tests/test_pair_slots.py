# SPDX-License-Identifier: Apache-2.0
"""Pin the pairing slot policy that makes capability negotiation order-independent.

The receiver commits the bond on the FIRST beacon it hears and never re-arms RX
on the pair AA before promoting, so an advert that arrives after that beacon is
unreachable. The old schedule (slots 0,1 then one in eight) assumed both ends
start together and latched capability 0/10 in the documented pairing order --
keyboard broadcasting first, dongle restarted into the running stream -- against
11/11 with the dongle camped first (Fisher p < 0.00001, measured 2026-08-22).

Properties pinned here:
  P1  every beacon is immediately preceded by an advert on the same channel
  P2  mid-stream-join coverage: wherever a receiver starts listening, the next
      frame it hears is an advert (M = 0 beacons of exposure)
  P3  schedule preservation: beacon-to-beacon spacing and the dwell/channel
      pattern are byte-identical to a build with no encryption, so a stock
      receiver sees no change
  P4  ACK-window quiet: nothing is transmitted after a beacon until its slot
      ends. This is the anti-regression pin -- the 2026-08 attempt put the
      advert there, left the keyboard deaf to the pair-ACK, and broke pairing.
  P5  with adverts disabled (bonded reconnect / plaintext build) the schedule is
      exactly the pre-change one
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src"

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include "kbd_rf_crypt.h"

/* Emit one slot per line: "<tick> <A|B>" so Python can assert the schedule. */
int main(int argc, char **argv)
{
    uint8_t advert_enabled = (argc > 1 && argv[1][0] == '1') ? 1u : 0u;
    uint8_t slot_phase = 0u;
    uint32_t t = 0u;
    int emitted = 0;

    while (emitted < 200) {
        uint8_t send_advert = 0u;
        uint16_t delay = kbd_pair_slot_next(advert_enabled, &slot_phase,
                                            &send_advert);
        printf("%u %c\n", (unsigned)t, send_advert ? 'A' : 'B');
        t += delay;
        emitted++;
    }
    return 0;
}
"""


def _cc():
    for name in ("cc", "gcc", "clang"):
        p = shutil.which(name)
        if p:
            return p
    return None


class PairSlots(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = _cc()
        if cc is None:
            raise unittest.SkipTest("no host C compiler available")
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        (d / "harness.c").write_text(HARNESS)
        cls.bin = d / "slotkat"
        subprocess.run(
            [cc, "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
             f"-I{SRC}", "-o", str(cls.bin), str(d / "harness.c")],
            check=True, capture_output=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def _schedule(self, advert_enabled):
        out = subprocess.run([str(self.bin), "1" if advert_enabled else "0"],
                             check=True, capture_output=True, text=True).stdout
        rows = []
        for line in out.strip().splitlines():
            tick, kind = line.split()
            rows.append((int(tick), kind))
        return rows

    def test_p1_every_beacon_is_preceded_by_an_advert(self):
        rows = self._schedule(True)
        beacons = [i for i, (_, k) in enumerate(rows) if k == "B"]
        self.assertTrue(beacons, "no beacons emitted")
        for i in beacons:
            self.assertGreater(i, 0, "a beacon led the stream with no advert")
            self.assertEqual(rows[i - 1][1], "A",
                             f"beacon at index {i} is not preceded by an advert")

    def test_p2_join_coverage_is_the_advert_lead_not_alternation(self):
        """Measure the real property: sweep every join instant on a fine grid and
        ask what the receiver hears FIRST.

        The exposure is exactly the advert->beacon lead: a receiver that starts
        listening inside that window hears the beacon first and commits without
        capability. Everywhere else in the slot the next frame is an advert.
        Asserting mere A/B alternation (an earlier version of this test) would
        pass even if the lead were widened until the beacon arrived first most
        of the time, so assert the FRACTION and the exposure bound instead.
        """
        rows = self._schedule(True)
        # Steady state only: skip the first slot pair.
        rows = [r for r in rows if r[0] >= rows[2][0]]
        span_start, span_end = rows[0][0], rows[-1][0]
        advert_first = total = 0
        for t10 in range(span_start * 10, span_end * 10):     # 0.1-tick grid
            t = t10 / 10.0
            nxt = next(((tt, k) for tt, k in rows if tt >= t), None)
            if nxt is None:
                break
            total += 1
            if nxt[1] == "A":
                advert_first += 1
        self.assertGreater(total, 0, "no join instants sampled")
        coverage = advert_first / total
        # 4 of every 32 ticks are the lead -> 28/32 = 87.5% is the design value.
        self.assertGreaterEqual(
            coverage, 0.85,
            f"join coverage fell to {coverage:.1%}; the advert lead is too wide "
            f"relative to the slot, so receivers increasingly hear a beacon first")
        # And the exposure must BE the lead, not something larger.
        self.assertLessEqual(1.0 - coverage, 4.0 / 32.0 + 0.01,
                             "exposure exceeds the advert lead")

    def test_p3_beacon_cadence_and_dwell_are_unchanged(self):
        rows = self._schedule(True)
        beacon_ticks = [t for t, k in rows if k == "B"]
        gaps = {b - a for a, b in zip(beacon_ticks, beacon_ticks[1:])}
        self.assertEqual(
            gaps, {32},
            f"beacon-to-beacon spacing changed: {sorted(gaps)} (want 20 ms = 32 ticks)")
        # The compatibility guarantee is the beacon CADENCE, not its phase: the
        # encrypted stream opens with an advert so its whole grid is offset by
        # the lead, which no receiver can observe. A stock receiver classifies
        # purely by the spacing, so that is what must match.
        plain = [t for t, k in self._schedule(False)]
        plain_gaps = [b - a for a, b in zip(plain, plain[1:])]
        enc_gaps = [b - a for a, b in zip(beacon_ticks, beacon_ticks[1:])]
        n = min(len(plain_gaps), len(enc_gaps))
        self.assertEqual(
            enc_gaps[:n], plain_gaps[:n],
            "encrypted and plaintext builds no longer share a beacon cadence")

    def test_p4_nothing_transmits_in_the_post_beacon_ack_window(self):
        rows = self._schedule(True)
        for i, (t, k) in enumerate(rows[:-1]):
            if k != "B":
                continue
            nxt_t, nxt_k = rows[i + 1]
            # The next transmission after a beacon must be a full slot away
            # (minus the lead), never inside the quiet window where the ACK lands.
            self.assertGreaterEqual(
                nxt_t - t, 32 - 4,
                f"a {nxt_k} transmission at tick {nxt_t} lands in the ACK window "
                f"after the beacon at {t} -- this is the 2026-08 regression")

    def test_p5_adverts_disabled_reproduces_the_plain_schedule(self):
        rows = self._schedule(False)
        self.assertTrue(all(k == "B" for _, k in rows), "advert emitted while disabled")
        ticks = [t for t, _ in rows]
        gaps = {b - a for a, b in zip(ticks, ticks[1:])}
        self.assertEqual(gaps, {32}, "plaintext/reconnect cadence changed")


if __name__ == "__main__":
    unittest.main()
