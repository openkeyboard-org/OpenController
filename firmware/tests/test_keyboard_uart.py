# Copyright 2026 Eric Molitor (EMulator)
# SPDX-License-Identifier: Apache-2.0
"""The UART frame parser, and the entropy-seed frame in particular.

Compiles the real `src/keyboard_uart.c` (polling build, no host wake) against
stub hardware headers and drives it with bytes and time. What is pinned here:

  - the seed frame is `9C 20 <32 bytes> <sum>`, the packet OpenDongle takes
    over USB as command 0x9C, and reaches the frame callback whole;
  - a seed frame that fails its checksum is dropped whole. Its body is 32
    arbitrary bytes, and the ordinary resynchroniser would otherwise run a
    command that happens to sit inside a damaged seed;
  - the same holds for a seed whose length byte is damaged: nothing after a
    0x9C is searched for headers until 35 bytes are in or the line goes quiet;
  - an incomplete frame is abandoned after 20 ms, so the parser cannot hold
    part of one for ever and keep the module from idling (a stray 0x9C costs
    the frames sent in that window, which the host retries);
  - a build without the key exchange does not know the frame at all.

Run: python3 -m unittest discover -s firmware/tests
"""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"

STUB_CONFIG_H = "/* stub */\n"

STUB_HAL_H = r"""
#ifndef STUB_HAL_H
#define STUB_HAL_H
#include <stdint.h>
#define FREQ_RTC 32768u
#define __INTERRUPT
#define __HIGH_CODE
#define ENABLE 1
#define DISABLE 0
#define UART_FIFO_SIZE 8
#define RB_LSR_OVER_ERR 0x02
#define RB_LSR_TX_ALL_EMP 0x40
#define RB_PIN_UART1 0
#define RB_IER_RECV_RDY 0
#define RB_IER_LINE_STAT 0
#define UART_1BYTE_TRIG 0
#define UART1_IRQn 0
#define bTXD1_ 0
#define bRXD1_ 0
#define bTXD1 0
#define bRXD1 0
#define GPIO_ModeOut_PP_5mA 0
#define GPIO_ModeIN_PU 0
#define GPIO_ModeIN_Floating 0
uint8_t h_rx_count(void);
uint8_t h_rx_pop(void);
extern uint8_t h_thr;
#define R8_UART1_RFC (h_rx_count())
#define R8_UART1_RBR (h_rx_pop())
#define R8_UART1_TFC 0
#define R8_UART1_LSR RB_LSR_TX_ALL_EMP
#define R8_UART1_THR h_thr
static inline void GPIOPinRemap(int a, int b) { (void)a; (void)b; }
static inline void GPIOA_SetBits(int a) { (void)a; }
static inline void GPIOB_SetBits(int a) { (void)a; }
static inline void GPIOB_ResetBits(int a) { (void)a; }
static inline void GPIOA_ModeCfg(int a, int b) { (void)a; (void)b; }
static inline void GPIOB_ModeCfg(int a, int b) { (void)a; (void)b; }
static inline void UART1_DefInit(void) {}
static inline void UART1_BaudRateCfg(int a) { (void)a; }
static inline void UART1_CLR_RXFIFO(void) {}
static inline void UART1_CLR_TXFIFO(void) {}
static inline void UART1_ByteTrigCfg(int a) { (void)a; }
static inline void UART1_INTCfg(int a, int b) { (void)a; (void)b; }
static inline void PFIC_EnableIRQ(int a) { (void)a; }
#endif
"""

# Line-driven: `feed <hex>` hands bytes to the UART and polls; `tick <n>`
# advances the RTC by n ticks (32768 Hz) and polls; `quiet` prints whether
# the parser holds nothing. Every dispatched frame prints one line.
HARNESS = r"""
#include "CONFIG.h"
#include "HAL.h"
#include "keyboard_uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t KeyboardUart_RxQuiet(void);

static uint8_t g_rx[512];
static unsigned g_head, g_tail;
static uint32_t g_now;
uint8_t h_thr;

uint8_t h_rx_count(void) { return (uint8_t)(g_head - g_tail); }
uint8_t h_rx_pop(void) { return g_rx[g_tail++]; }
uint32_t RtcClock_Now(void) { return g_now; }

static void on_frame(uint8_t cmd, uint8_t sub, const uint8_t *payload, uint8_t len)
{
    printf("frame %02x %02x %u ", cmd, sub, len);
    for (uint8_t i = 0; i < len; i++) printf("%02x", payload[i]);
    printf("\n");
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(void)
{
    char line[2048];

    KeyboardUart_Init();
    KeyboardUart_SetFrameCallback(on_frame);
    while (fgets(line, sizeof(line), stdin)) {
        if (!strncmp(line, "feed ", 5)) {
            const char *s = line + 5;
            g_head = g_tail = 0;
            while (hexval(s[0]) >= 0 && hexval(s[1]) >= 0) {
                g_rx[g_head++] = (uint8_t)((hexval(s[0]) << 4) | hexval(s[1]));
                s += 2;
            }
            KeyboardUart_Poll();
        } else if (!strncmp(line, "tick ", 5)) {
            g_now += (uint32_t)strtoul(line + 5, 0, 10);
            KeyboardUart_Poll();
        } else if (!strncmp(line, "quiet", 5)) {
            printf("quiet %u\n", KeyboardUart_RxQuiet());
        }
    }
    return 0;
}
"""

MS = 33          # RTC ticks in a millisecond, rounded up


def seed_frame(seed: bytes) -> bytes:
    body = bytes([0x9C, 0x20]) + seed
    return body + bytes([sum(body) & 0xFF])


def a6(sub: int) -> bytes:
    return bytes([0xA6, sub, (0xA6 + sub) & 0xFF])


class KeyboardUartParser(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc") or shutil.which("gcc")
        if cc is None:
            raise unittest.SkipTest("no host C compiler")
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = Path(cls.tmp.name)
        (tmp / "CONFIG.h").write_text(STUB_CONFIG_H)
        (tmp / "HAL.h").write_text(STUB_HAL_H)
        (tmp / "harness.c").write_text(HARNESS)
        cls.exe = {}
        for kex in (0, 1):
            exe = tmp / f"uart_kex{kex}"
            subprocess.run(
                [cc, "-std=gnu99", "-O1", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", "-Wno-unused-variable",
                 f"-DKBD_KEX={kex}", "-DKBD_IDLE_WFI=0", "-DKBD_HOST_WAKE=0",
                 "-DKBD_DEEP_SLEEP=0", "-DKBD_UART1_REMAP=1",
                 "-I", str(tmp), "-I", str(SRC),
                 str(tmp / "harness.c"), str(SRC / "keyboard_uart.c"),
                 "-o", str(exe)],
                check=True)
            cls.exe[kex] = exe

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_script(self, lines, kex=1):
        out = subprocess.run([str(self.exe[kex])], input="\n".join(lines) + "\n",
                             capture_output=True, text=True, check=True)
        return out.stdout.split("\n")[:-1]

    def test_a_seed_frame_reaches_the_callback_whole(self):
        seed = bytes((0x11 * i + 3) & 0xFF for i in range(32))
        out = self.run_script([f"feed {seed_frame(seed).hex()}", "quiet"])
        self.assertEqual(out, [f"frame 9c 00 32 {seed.hex()}", "quiet 1"])

    def test_a_seed_frame_split_across_polls_is_one_frame(self):
        seed = bytes(range(0x40, 0x60))
        frame = seed_frame(seed).hex()
        out = self.run_script([f"feed {frame[:20]}", f"tick {MS}",
                               f"feed {frame[20:]}", "quiet"])
        self.assertEqual(out, [f"frame 9c 00 32 {seed.hex()}", "quiet 1"])

    def test_a_damaged_seed_does_not_run_the_command_inside_it(self):
        # A6 52 F8 is a well-formed unpair. With the frame's checksum off by
        # one, the resynchroniser used to find it and dispatch it.
        seed = bytes([0xA6, 0x52, 0xF8]) + bytes([0x11] * 29)
        good = seed_frame(seed)
        bad = good[:-1] + bytes([(good[-1] - 1) & 0xFF])
        out = self.run_script([f"feed {bad.hex()}", "quiet",
                               f"feed {a6(0x53).hex()}"])
        self.assertEqual(out, ["quiet 1", "frame a6 53 0 "])
        # And undamaged it is a seed, not an unpair.
        out = self.run_script([f"feed {good.hex()}"])
        self.assertEqual(out, [f"frame 9c 00 32 {seed.hex()}"])

    def test_a_seed_with_a_damaged_length_does_not_run_the_command_inside_it(self):
        seed = bytes([0xA6, 0x52, 0xF8]) + bytes([0x11] * 29)
        good = seed_frame(seed)
        bad = good[:1] + bytes([0x21]) + good[2:]
        out = self.run_script([f"feed {bad.hex()}", "quiet",
                               f"feed {a6(0x53).hex()}"])
        self.assertEqual(out, ["quiet 1", "frame a6 53 0 "])
        # Cut short as well: the line going quiet abandons it, unparsed.
        out = self.run_script([f"feed {bad[:20].hex()}", f"tick {25 * MS}", "quiet",
                               f"feed {a6(0x53).hex()}"])
        self.assertEqual(out, ["quiet 1", "frame a6 53 0 "])

    def test_a_stray_header_is_given_up_when_the_line_goes_quiet(self):
        # The host's three tries of a query, 20 ms apart, behind one stray
        # byte: none is searched for (they could be a seed's body). Once the
        # host stops, the parser lets go and the next frame is taken.
        q = a6(0x5A).hex()
        out = self.run_script([f"feed 9c{q}", f"tick {19 * MS}", f"feed {q}",
                               f"tick {19 * MS}", f"feed {q}", "quiet",
                               f"tick {25 * MS}", "quiet", f"feed {q}"])
        self.assertEqual(out, ["quiet 0", "quiet 1", "frame a6 5a 0 "])

    def test_an_incomplete_frame_is_abandoned_after_twenty_milliseconds(self):
        seed = bytes(range(32))
        part = seed_frame(seed)[:12].hex()
        out = self.run_script([f"feed {part}", "quiet", f"tick {10 * MS}", "quiet",
                               f"tick {15 * MS}", "quiet",
                               f"feed {a6(0x53).hex()}"])
        self.assertEqual(out, ["quiet 0", "quiet 0", "quiet 1", "frame a6 53 0 "])

    def test_the_timeout_runs_from_the_last_byte_not_the_first(self):
        seed = bytes(range(0x80, 0xA0))
        frame = seed_frame(seed).hex()
        out = self.run_script([f"feed {frame[:10]}", f"tick {15 * MS}",
                               f"feed {frame[10:40]}", f"tick {15 * MS}",
                               f"feed {frame[40:]}"])
        self.assertEqual(out, [f"frame 9c 00 32 {seed.hex()}"])

    def test_a_partial_keyboard_report_expires_too(self):
        out = self.run_script(["feed a10000", f"tick {25 * MS}", "quiet",
                               f"feed {a6(0x53).hex()}"], kex=0)
        self.assertEqual(out, ["quiet 1", "frame a6 53 0 "])

    def test_a_build_without_the_exchange_does_not_know_the_frame(self):
        seed = bytes([0x11] * 31 + [0x12])
        out = self.run_script([f"feed {seed_frame(seed).hex()}", "quiet"], kex=0)
        self.assertEqual(out, ["quiet 1"])


if __name__ == "__main__":
    unittest.main()
