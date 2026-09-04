/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART diag dump (bench tool). minichlink resets the CH592 on every SWD
 * access, so post-hoc counter reads over SWD are a fresh boot's values; this
 * path returns the .diag_safe counters over the keyboard UART instead.
 * Compiled to ACK-only no-ops when RF_DIAG_COUNTERS=0. Refused (empty frame)
 * while CONNECTED: a ~65-byte bounded-spin TX would disturb the hop grid.
 * Allowed in PAIRING but perturbative (~4 ms main-loop block: one beacon may
 * be delayed / one reply missed): treat as a one-shot probe, do not poll it
 * during a search. Feature knob KBD_UART_DIAG_DUMP (default = RF_DIAG_COUNTERS).
 */
#ifndef DIAG_DUMP_H
#define DIAG_DUMP_H
void DiagDump_Send(void);   /* A6 71 */
void DiagDump_Zero(void);   /* A6 72: zero counters, keep ll_boot_count */
#endif
