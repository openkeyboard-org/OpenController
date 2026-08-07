# TMOS / CH592 SDK Alignment Review

**Subject:** This firmware (`src/`) vs. the canonical WCH CH592 BLE/RF SDK ("TMOS") conventions.
**Question:** Where are we aligned with WCH best practice, where have we diverged, and which
divergences are *opportunities* vs. *justified design choices*? Special attention to the
`bb_irq_trampoline.S` interrupt trampoline.
**Scope:** Consultant review only — **no code is changed here**. This document records issues,
improvements, and refactoring opportunities. (It originally complemented code-review and
stock-difference logs that remain in the private prototype tree.)

**Method.** Compared against the authoritative WCH CH592 EVT SDK
— examples `EXAM/BLE/RF_PHY`, `RF_PHY_Hop`; `EXAM/BLE/LIB/ble_task_scheduler.S`;
`EXAM/BLE/HAL/MCU.c`; `EXAM/SRC/{Startup,Ld,StdPeriphDriver,RVMSIS}`. Library symbols
confirmed by `nm` of `vendor-ble-v1.00/LIBCH59xBLE.a`. Conclusions were cross-checked by an
independent Codex read-only review and corrected where it pushed back (noted inline).

> **RESOLUTION (2026-06-30, bench-verified) — supersedes the §2 headline below.**
> A follow-up investigation (branch `investigate/bb-vs-lle`) **refuted** the claim that the
> BB trampoline is *required*. `src/bb_irq_trampoline.S` was **dead code**: the BLE library
> fast-vectors the BLEB baseband IRQ in hardware via a Qingke-V4 PFIC fast-vector
> (`BB_DevInit` writes `IDCFGR[3]=20`, `FIADDRR[3]=BB_IRQLibFunction|1`), bypassing the
> software vector table. **Bench proof:** with the trampoline present, `bb_irq_count` stayed
> **0** while RF callbacks fired (259/259 TX-finish). The trampoline was **removed** and full
> pairing + **connected HID delivery** validated (32 reports to host) on **both** the v1.00 and
> v1.4.2 libraries. The firmware now ships no app BB handler, matching the SDK **LWNS** example.
>
> The manual `PFIC_EnableIRQ(BLEB/BLEL)` in `main.c` were also **removed** — a follow-up
> OpenOCD re-validation (2026-06-30) showed they are **NOT required**: with both removed the
> keyboard connects, delivers 100% HID, and runs 348,867 connected hops with **0 HardFaults**.
> The library's own `BLE_IPCoreInit` already writes the IRQ 20/21 enables, so the app-side calls
> were redundant. The §2 analysis is preserved for history; **P1 is DONE**.
>
> **Bigger correction (2026-06-30):** the whole "v1.4.2 faults TMOS during RF" premise was a
> **minichlink measurement artifact**, not a runtime bug. minichlink's SDI reset deterministically
> induces the TMOS misaligned-load HardFault *at boot* (a sentinel written to the `0x20005804`
> marker is overwritten by `0xDE` on the next minichlink reset); OpenOCD `reset halt` is clean and
> an unwatched 15-min connected soak took 0 faults with 1M+ clean hops. Debug this part with the
> **WCH MRS OpenOCD fork** (`-c "chip_id CH59x"`), **not minichlink** — see README "Debugging".
>
> *Lesson — three separate things, not one.* (1) The BB **trampoline** was never needed (the lib
> fast-vectors BLEB); the 2026-05-17 "BB required" belief conflated adding it with adding the PFIC
> enables, and a stale `0xDE` fault marker reinforced the error. (2) The PFIC **enables** are NOT
> needed either — "required on v1.4.2" was itself a minichlink-reset artifact. (3) The v1.4.2 TMOS
> HardFault does not occur in real operation; it only appears under debugger perturbation
> (minichlink SDI reset, or an OpenOCD halt/resume).

---

## 1. Executive summary

The firmware is a **clean, SDK-faithful, event-driven TMOS port**. The TMOS task model, the
`Main_Circulation`/`TMOS_SystemProcess` loop, the init order, the `rfConfig_t` setup, and the
"defer work out of the ISR into TMOS events" discipline all match the canonical examples
closely. The startup CSR/HPE/`mtvec` setup is byte-for-byte the stock sequence.

There **was** one genuinely hand-rolled interrupt path — `BB_IRQHandler` (`bb_irq_trampoline.S`).
Follow-up investigation proved it **dead code** and **removed** it (see the RESOLUTION banner
above): the BLE library fast-vectors the BLEB IRQ in hardware, so no app handler is needed —
matching the SDK LWNS example. The 124 lines of assembly are gone. (The manual
`PFIC_EnableIRQ(BLEB/BLEL)` were also **removed** — OpenOCD re-validation showed they are *not*
required; the "required on v1.4.2" reading was a minichlink-reset artifact. See the RESOLUTION
banner.)

The other notable divergences — a hand-rolled RTC-timed channel hop and extra non-TMOS work in
the main loop — are **deliberate and protocol-driven** (they exist to mirror the stock dongle's
proprietary master-poll/slave-respond link, which is *not* what the SDK's RF demos do). They are
justified; the opportunity there is to **document the invariants**, not to "fix" them.

---

## 2. Headline finding — the `BB_IRQHandler` trampoline

> ⚠️ **SUPERSEDED — historical.** This section's intermediate conclusion ("BB is required for
> this firmware's basic-mode path") was **wrong**; see the RESOLUTION banner at the top. The BB
> interrupt is serviced by the library's hardware fast-vector, the trampoline was dead code, and
> it has been removed. The analysis below is retained to show how the investigation progressed.

> *"The `.S` file for the interrupt trampoline is suspicious — I cannot imagine the SDK is not
> doing this for us somehow."*

**Short answer: for the `RF_PHY`/`RF_PHY_Hop` demos it *does* do it for you — through the LLE
interrupt, not BB. This firmware took a different (basic-mode raw-RX) RF path and, as built, needs
BB. So the *wiring* is justified as it stands; the *form* (hand-written assembly) is the clear
opportunity — and there is an open question (the LWNS caveat below) about whether the BB handler
is needed at all, since the SDK's own basic-mode example does without it.**

### What the library actually exports
`nm vendor-ble-v1.00/LIBCH59xBLE.a`:

| Symbol | Status | Meaning |
|---|---|---|
| `BB_IRQLibHandler` | **T** (defined) | the baseband worker — a normal returning C function |
| `LLE_IRQLibHandler` | **T** (defined) | the link-layer worker |
| `g_LLE_IRQLibHandlerLocation` | **U** (undefined) | a function pointer the *app* must define |
| `BB_IRQHandler` (vector entry) | — (absent) | the lib provides **no** vector-named BB handler |

So the library ships the *worker* (`BB_IRQLibHandler`) but never the *vector entry*
(`BB_IRQHandler`). Something outside the lib has to connect the BLEB vector to the worker.

### How the canonical example connects it — via LLE, in AUTO mode
The stock `RF_PHY` example builds with `RF_AUTO_MODE_EXAM = 1`, i.e.
`rf_Config.LLEMode = LLE_MODE_AUTO` (`EVT/.../RF_PHY/APP/RF_PHY.c:22,262`). In AUTO mode the
**LLE engine** drives the radio and raises the **BLEL/LLE** interrupt. That vector is wired by:

- `EVT/.../BLE/LIB/ble_task_scheduler.S:57` — a strong `LLE_IRQHandler` assembly trampoline that
  dispatches through the runtime function pointer `g_LLE_IRQLibHandlerLocation`, and
- `EVT/.../BLE/HAL/MCU.c:18,118` — `g_LLE_IRQLibHandlerLocation = (uint32_t)LLE_IRQLibHandler;`

In that configuration **`BB_IRQHandler` is never overridden** — it stays the weak `j 1b` stub in
`startup_CH592.S:45,76` and is simply never used. *This is the "the SDK does it for you" case.*

### Why this firmware needs BB anyway
This firmware does **not** use AUTO mode. To mirror the stock proprietary protocol it configures
**basic-mode continuous raw RX**:

- `rf_task.c:17` — `RF_LLE_MODE = LLE_MODE_BASIC | LLE_WHITENING_ON | LLE_MODE_PHY_2M`
- `rf_task.c:509` — `RF_Rx(NULL, 0, 0xFF, 0xFF)` (raw, un-parsed receive)
- `main.c:151–152` — enables **both** `BLEB_IRQn` and `BLEL_IRQn`

In this raw basic-mode path, the RX/TX-complete events surface on the **BB (baseband)**
interrupt, and `RF_2G4StatusCallBack` does not fire without a strong `BB_IRQHandler`. That was
bench-proven (2026-05-17: `rf_cb_count[*]` stayed 0 until the BB handler was added). **So wiring
`BB_IRQHandler → BB_IRQLibHandler` is correct and bench-required for this build** — though, as the
caveat below shows, that necessity is itself worth interrogating rather than taken as inherent.

> **Important caveat — the SDK's own basic-mode example needs no BB handler.** The EVT SDK's LWNS
> examples use the *same* configuration as this firmware — `rfConfig.LLEMode = LLE_MODE_BASIC` and
> `RF_Rx(NULL, 0, …)` (`EVT/.../LWNS/APP/lwns_adapter_no_mac.c:200,253`) — yet ship **no**
> `BB_IRQHandler`, no `PFIC_EnableIRQ(BLEB_IRQn)` in the app, and no custom `ble_task_scheduler`:
> they run on the stock LLE/HAL wiring alone. So "basic-mode raw RX requires a BB handler" is
> **this firmware's bench-proven behavior, not a general SDK rule.** That divergence is itself a
> lead (see P1): the custom BB handler may be compensating for an init step LWNS gets from the
> standard path (e.g. inside `CH59x_BLEInit`/HAL, or a BLE-lib-version difference), in which case
> the cleanest alignment is to *remove* the custom BB handler entirely rather than rewrite it.

### The opportunity: assembly → C
`bb_irq_trampoline.S` saves **all** of `x1, x4–x31` (including callee-saved `s0–s11`), calls
`BB_IRQLibHandler`, restores everything, and `mret`s — 124 lines. That full save is **redundant
for BB**:

- `BB_IRQLibHandler` is an ordinary C function, so it already preserves callee-saved registers
  (`s0–s11`) per the RISC-V ABI — saving them in the trampoline is pure overhead.
- The caller-saved set is handled by **HPE** (hardware prologue/epilogue), enabled at startup via
  `CSR 0x804 = 0x3` (`startup_CH592_phased.S:222`).
- The SDK's interrupt idiom is `__INTERRUPT == __attribute__((interrupt("WCH-Interrupt-fast")))`
  (`StdPeriphDriver/inc/CH59x_common.h`), which emits exactly the right HPE-aware
  prologue/epilogue for an ISR that calls another function.

**This firmware already uses that idiom — for `TMR0_IRQHandler`** (`rf_task.c:685`,
`__INTERRUPT __HIGH_CODE`). The BB handler can be the same shape:

```c
__INTERRUPT
__HIGH_CODE
void BB_IRQHandler(void) {
#if RF_DIAG_COUNTERS
    bb_irq_count++;
#endif
    BB_IRQLibHandler();
}
```

**Nuances (so this isn't overstated):**
- The full-save assembly frame is **not bizarre** — WCH's *own* `LLE_IRQHandler`
  (`ble_task_scheduler.S:58`) uses the identical full-save frame. But it does so because of the
  runtime `g_LLE_IRQLibHandlerLocation` indirection and the BLE scheduler/ecall machinery —
  **neither of which applies to BB.** `BB_IRQLibHandler` is a direct symbol called once.
- The trampoline comment's claim — *"software save/restore is required even though HPE is on,
  because the hardware-stacked region is only restored on mret"* — does **not** make the C form
  unsafe. That the interrupted context is restored on `mret` is precisely *why* a
  `WCH-Interrupt-fast` C handler that calls a normal C function is correct by construction: the
  called function may clobber caller-saved registers, and `mret` restores the interrupted
  context afterward.
- Because this is timing-sensitive RF code with an empirical track record, treat the swap as a
  **measured change**: A/B bench it (see P1) rather than assuming equivalence.

---

## 3. Where we are ALIGNED with SDK / TMOS best practice

| Area | This firmware | Canonical SDK | Verdict |
|---|---|---|---|
| Single TMOS task; event-bitmask dispatch; `events ^ BIT` return; `SYS_EVENT_MSG` drain | `rf_task.c:714` (`TMOS_ProcessEventRegister`), `:376` (`RF_ProcessEvent`) | `RF_PHY.c:198–241,256` | ✅ idiomatic |
| Main loop: `__HIGH_CODE __attribute__((noinline)) Main_Circulation` running `TMOS_SystemProcess()` | `main.c:128,131` | `RF_main.c:37–43` | ✅ matches |
| BLE/TMOS init subsequence `CH59x_BLEInit → HAL_Init → RF_RoleInit` (app inserts `KeyboardUart_Init` after `SetSysClock`, before BLE init) | `main.c:140–156` | `RF_main.c:52–74` | ✅ subsequence matches |
| `rfConfig_t` zeroed via `tmos_memset`, then `RF_Config` (AA / CRCInit / rfStatusCB / RxMaxlen) | `rf_task.c:480` (`rf_configure`) | `RF_PHY.c:250–268` | ✅ matches |
| **ISR handler written as idiomatic C `__INTERRUPT __HIGH_CODE`** (`TMR0_IRQHandler`) | `rf_task.c:685` | SDK `__INTERRUPT` idiom | ✅ — *and the template for fixing BB* |
| RF status callback defers heavy work to TMOS events; no blocking in the ISR | `rf_task.c:245` (`RF_2G4StatusCallBack`) | SDK callback pattern | ✅ good discipline |
| Startup CSR setup: `0xbc0=0x1f`, `0x804=0x3` (HPE + nesting), `mstatus=0x88`, `mtvec|3` | `startup_CH592_phased.S:219–230` | byte-identical to stock `startup_CH592.S` | ✅ faithful |
| Interrupt critical section correctly masks/restores global IRQ state (saves prior `0x88` bits, not an unconditional re-enable) | `rf_task.c:213,219` | mirrors `core_riscv.h` `__risc_v_disable_irq/__enable_irq` | ✅ correct |

---

## 4. Where we DIVERGE

For each: **what**, **why**, and **verdict** (justified vs. opportunity).

**D1 — BB interrupt wiring (headline).** *What:* earlier builds supplied a strong app
`BB_IRQHandler` (`bb_irq_trampoline.S`). *Verdict:* **RESOLVED — removed.** The BLE library
fast-vectors the BLEB IRQ in hardware (`BB_DevInit`), so no app handler is needed; bench-validated
on v1.00 and v1.4.2 (see the RESOLUTION banner). Matches the SDK LWNS example.

**D2 — Channel hopping.** *What:* a hand-rolled RTC32K time-based hop in `RF_ConnectedTick`
(`rf_task.c:576`) instead of the SDK's `RF_FrequencyHoppingRx/Tx` + `LLE_MODE_AUTO` +
`ChannelMap`/`HeartPeriod` (`RF_PHY_Hop`). *Why:* the stock protocol's phase-locked
park-and-follow hop is not the SDK's auto-hop demo; the library's hopping primitives don't model
it. *Verdict:* **justified.** Opportunity: note explicitly in code/docs that the SDK hop
primitives are intentionally unused, so a future reader doesn't "modernize" it and break the
phase lock.

**D3 — Extra non-TMOS work in the main loop.** *What:* `Main_Circulation` also calls
`RF_ConnectedTick()`, `KeyboardUart_Poll()`, and `WATCHDOG_FEED()` (`main.c:130–135`), where the
canonical loop is just `TMOS_SystemProcess()`. *Why:* the hop phase-tracking needs finer
granularity than TMOS's ~625 µs tick, so it must run every loop iteration. *Verdict:* **justified,
with a real coupling risk** — loop latency is now in the RF timing path: `KeyboardUart_Poll`
drains the UART RX FIFO (`keyboard_uart.c:173`) and the frame handling reached from it can perform
bounded UART TX, so a stall on that path would delay the hop. This is already mitigated (R5
bounded the UART TX spin and added the WWDG). Opportunity: state the "main loop must not block"
invariant where it can be seen (it's currently implicit).

**D4 — CSR critical section is raw inline asm.** *What:* `rf_task.c:213,219` hand-code
`csrrc/csrrs 0x800, 0x88`. *Verdict:* **correct but low-readability.** Opportunity: call the named
SDK helpers `__risc_v_disable_irq()` / `__risc_v_enable_irq()` (`core_riscv.h:110,126`) instead of
literal CSR ops. *Correction to an earlier draft:* `SYS_DisableAllIrq` / `SYS_RecoverIrq` are
**not** the right substitute — they save/restore the **PFIC peripheral enable masks**, not the
global machine-interrupt CSR state (`CH59x_sys.c:158,175`). The inline asm is the correct
primitive; only its spelling is the nit.

**D5 — RF reconfigured before each TX/RX.** *What:* `rf_configure()` is re-invoked around
operations (`rf_task.c:508,529`) rather than the SDK's configure-once-at-init. *Why:* robustness
of the RX window (documented in `STOCK_DIFFERENCES.md`). *Verdict:* **justified tradeoff**; worth
a one-line note on the cost (extra setup per operation) for release tuning.

**D6 — Diagnostic startup/linker/build scaffolding.** *What:* phased boot markers (`0xC0–0xC5`),
the `.diag_safe` SRAM region, a fault handler that **spins** (`fault_handler.S`) instead of
resetting, FLASH origin at `0x1000`, and a 4× stack. *Why:* bring-up diagnosis of the
reset-on-RF-activity blocker; the `0x1000` origin and size cap protect the vendor boot trampoline
/ IAP region. *Verdict:* **justified for development.** Opportunity: gate the *diagnostic* pieces
(markers, fault-spin, `.diag_safe`) behind a `DIAG` build flag so a release build uses the stock
startup and a reset-on-fault policy (a spinning fault handler in the field just hangs the module).

---

## 5. Prioritized opportunities

**P1 — Resolve the BB handler: first ask whether it's needed at all, then simplify if it is.**
*(high value)* — ✅ **DONE (2026-06-30, branch `investigate/bb-vs-lle`): the BB handler was not
needed at all. `src/bb_irq_trampoline.S` was removed (VTF services BB); the manual PFIC enables
were *also* removed (re-validated under OpenOCD as NOT required — the "required by v1.4.2" reading
was a minichlink-reset artifact); connected HID delivery validated on both libs. See the
RESOLUTION banner at the top.**
- **(a) Investigate first.** The SDK's LWNS example does basic-mode raw RX with no BB handler (§2
  caveat). Diff this firmware's `CH59x_BLEInit`/`HAL_Init` path, BLE-lib version, and
  `PFIC_EnableIRQ(BLEB_IRQn)` usage against LWNS to find *why* BB is required here. If it is a
  missing/again-set init step from the standard path, the cleanest alignment is to **delete
  `bb_irq_trampoline.S` entirely** and rely on the stock LLE/HAL wiring.
- **(b) If BB must stay**, replace the 124-line assembly with the 4-line C `__INTERRUPT
  __HIGH_CODE` handler (same form as `TMR0_IRQHandler`; keep the diag counter as a C `++`). This
  removes ~120 lines of assembly and directly answers the original suspicion.

Either way, **bench-verify** (timing-sensitive; the project's empirical culture): build
`make BLE_LIB_DIR=vendor-ble-v1.00 RF_DIAG_COUNTERS=1`, confirm `bb_irq_count` / `rf_cb_count[*]`
behavior, and re-run a connected-mode soak to confirm 100% delivery is retained.

**P2 — Document the deliberate divergences and name the CSR helpers.** *(clarity)* (a) ~~note why
BB must be wired~~ — RESOLVED in D1 (no app BB handler; lib fast-vectors BLEB); document why the
hop is hand-rolled (D2); (b) make the "main loop must never block" invariant explicit (D3); (c)
swap the raw CSR ops for `__risc_v_disable_irq/__enable_irq` (D4).

**P3 — Separate diagnostic scaffolding from a release build.** *(release hygiene)* DIAG-gate the
phased markers, the spinning fault handler (→ reset in release), and `.diag_safe` (D6); revisit
the per-operation RF reconfigure cost (D5).

---

## 6. Appendix — evidence & cross-references

**Library symbols** (`nm vendor-ble-v1.00/LIBCH59xBLE.a`): `BB_IRQLibHandler`,
`LLE_IRQLibHandler`, `LLE_IRQHandler` defined (T); `g_LLE_IRQLibHandlerLocation` undefined (U);
**no** `BB_IRQHandler`.

**Canonical-vs-here interrupt wiring:**
- SDK AUTO (`RF_PHY`/`RF_PHY_Hop`): BLEL/LLE vector → `ble_task_scheduler.S` (`LLE_IRQHandler`) →
  `*g_LLE_IRQLibHandlerLocation` → `LLE_IRQLibHandler`; BB weak/unused.
- SDK basic raw RX (`LWNS`, same `LLE_MODE_BASIC` + `RF_Rx(NULL,0,…)` as here): **no** app
  `BB_IRQHandler`, no `PFIC_EnableIRQ(BLEB)` — runs on the stock LLE/HAL wiring alone.
- Here (basic raw RX, after this change): **no app `BB_IRQHandler`** — the lib's hardware
  fast-vector (`BB_DevInit`: `FIADDRR[3]=BB_IRQLibFunction|1`) routes BLEB to `BB_IRQLibFunction`,
  same as LWNS. BLEL/LLE serviced by `ble_task_scheduler.S`. The app no longer keeps
  manual `PFIC_EnableIRQ(BLEB/BLEL)` calls; the earlier "required by v1.4.2" reading was
  a debugger-reset artifact. The P1 question is resolved.

**Relationship to prior reviews:** this review is orthogonal to `CODE_REVIEW_2026-06-29.md`
(application logic R1–R13, mostly resolved) and `STOCK_DIFFERENCES.md` (binary-level divergences). The items
here concern *SDK/TMOS alignment* and were not separately tracked there; D1/P1 (the trampoline)
is the new, highest-value finding.

**Status:** recommendations only — no code modified by this review.
