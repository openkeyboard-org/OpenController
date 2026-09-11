# OpenController resting policy: design sketch (2026-09-11)

Status: decisions taken 2026-09-11 (values may be tuned as measurements come in); stage 1 in progress. Numbers are PPK2 readings taken today on the
OpenDongle bench (calibrated, 100 kS/s, `OpenDongle/tools/bench/ppk2d.py`), against the dongle's
own state counters. Companion: `POWER_MANAGEMENT_COMPARISON.md`, whose item 2 this is.

## Why

The dongle's Tier 2 savings exist only against a keyboard that rests. Its windowed receiver and
halt engage on a scheduled-drop cadence or in the bonded camp; a keyboard that holds its link keeps
the dongle in continuous receive. OpenController holds its link. Measured today, dongle side:

| Dongle state | Host awake | Host asleep |
|---|---|---|
| connected, OpenController holding the link | **11.48 mA** (floor 11.0, poll bursts 13.9 mA on the 0.88 ms slot) | ~10.7 (Tier 1 S figure) |
| windowed camp, no keyboard traffic | **5.09 mA** (floor 4.1, 30 ms windows at 10.9 mA every 200 ms) | **1.35 mA** (R3b, halting between windows; measured with the production keyboard) |

A resting OpenController buys the right-hand column for our own keyboard, and lets the CH592
controller deep-sleep between probes, which is the keyboard-battery win this bench cannot yet
measure (the PPK2 is on the dongle's feed).

## What the dongle already accepts (no dongle change needed)

- Detector window: promote-to-promote cadence **900..1100 ms**, a drop counts as scheduled when
  the link lapses **< 300 ms** after promote and carried **>= 1 answered poll**; phase window
  lead 45 ms (3 sigma of the production keyboard's 15 ms jitter); supervision lapses ~19 ms after
  the last RX.
- Today, emulating the cadence from the keyboard MCU through the QMK host switch, the detector
  **locked on OpenController's reconnects: 16 catches on 16 phase windows, 0 misses.**
- The emulation also showed the trap: each emulated probe held the link **239 ms** (the UART
  protocol's release dwell and round trips), and the resting mean was 8.30 mA. Production's probe
  is ~10 ms. Rest must be native to the controller and invisible to the UART protocol.

## Proposed behaviour (controller firmware)

1. **Activity** = a validated A1 report from the keyboard MCU (production counts validated UART
   frames; host `61 0D 0A` acks do not count). After **T_idle** without activity, enter REST.
2. **REST entry**: stop answering polls. No frame to the dongle; it lapses by supervision into its
   bonded camp. Keep the bond. Keep the keyboard MCU unaware: continue to report connected, do not
   send `5B 33` (the QMK driver reconnects on any reported loss and would fight the rest).
3. **REST loop**: every **T_probe** do a bonded reconnect probe (`RF_Select2G4`, bonded flavor,
   the existing fast attempt), answer >= 1 poll (this is where the dongle's LEN-3 LED relay and
   host status arrive), then go quiet again with the whole probe **<= 20 ms** promote-to-quiet.
   Between probes: RF IDLE, `RF_CanDeepSleep()` true, the existing deep sleep under the TMOS
   idleCB contract with the RTC deadline set to the next probe and the UART-RX GPIO wake armed.
4. **Key during REST**: UART RX wakes the part; the queued report triggers an immediate probe;
   deliver; stay CONNECTED; restart T_idle. Dongle-side cost: its 30 ms windows every 200 ms bound
   the first-key latency at ~200 ms worst, ~100 ms typical, plus the ~5 ms reconnect. The
   phase-locked window helps scheduled probes only. Reducing `PM_RX_PERIOD_MS` to 100 ms would
   halve that at roughly +1 mA awake (floor 4.1 + 30 % of 6.8).
5. **Stage 2 (production parity, owner-confirmed)**: after **T_deep = 30 min** in stage 1 stop
   probing; deep sleep until a key (UART-RX GPIO wake), no RTC wake. Costs LED/host-status updates
   until the next key; the dongle keeps windowing in its camp and halting under suspend, its phase
   window simply stops catching.
6. **Host asleep**: nothing extra on the keyboard side; the dongle halts between windows and the
   phase window catches the probes (this is the R3b path the fifth gate exercised).

Dongle-side knob to revisit once the keyboard rests natively: `PM_RX_GRACE_S` (60 s of continuous
scan after every genuine session before windowing; each cadenced probe also decrements it by one
period). A shorter grace trades a slower reconnect after a pause for less time at 10.4 mA.

## Owner decisions

| # | Decision | Options / proposal |
|---|---|---|
| D1 | T_idle | **Decided: 5 s (production parity), configurable; may be tuned.** |
| D2 | T_probe | **Decided: 1010 ms**, jitter <= 15 ms (inside the dongle's 900-1100 window with the 45 ms phase lead). |
| D3 | Probe dwell | **Decided: target <= 20 ms** promote-to-quiet, measured on the PPK2 raw trace. Production ~10 ms. |
| D4 | MCU visibility | **Decided: transparent.** The MCU keeps seeing a connected link. |
| D5 | Stage 2 | **Decided: include, T_deep = 30 min (production parity).** Built after stage 1 is measured. |
| D6 | Dongle window period | **Decided: keep 200 ms**; measure the first-key latency (gate 4), revisit if it is noticeable. |
| D7 | Bench | Move the PPK2 to the controller board's feed for one rung to get the keyboard-side number. |

## Probe-dwell finding (bench, 2026-09-11)

Measured on the PPK2 (dongle 3V3 feed, host awake), first working build:

| State | Dongle current |
|---|---|
| connected, controller holding the link | 11.5 mA |
| **resting, 1.010 s probes** | **5.74 mA** (floor 2.6-4.1 mA; the dongle now also halts between some windows even host-awake) |
| windowed camp, keyboard absent | 5.09 mA |

The probe adds ~0.65 mA over the windowed floor, and **this is dongle-bounded, not a
keyboard-side dwell**:

- The dongle counters show 1 promote/s and ~2 answered polls/s: the keyboard-side probe is
  ~2 polls (~2 ms). The full-rate trace's ~50-120 ms "burst" is the *dongle's* reaction, not our TX.
- `KBD_REST_PROBE_ANSWERS` 2 -> 1 leaves the average unchanged (5.746 vs 5.747 mA): the cost is
  not the number of polls we answer.
- The cost is the dongle catching the probe in an extra ~30 ms phase window, then polling for its
  supervision timeout after we vanish (`600/20 = 18.8 ms`, `rf_supervision.h`), then a *windowed*
  ev10 reacquire the locked detector returns to camp. ~30 ms window + ~19 ms connect ~= 0.35 mA.

**Conclusion: 5.74 mA host-awake is essentially the windowed floor plus one unavoidable
catch/second, and matches what any resting keyboard (production included) yields with this dongle
firmware.** There is no further keyboard-side lever on the probe. Shrinking it would be an
OpenDongle change (shorter connected-supervision, or recognizing a scheduled 1-poll probe and
dropping to camp without the ~19 ms poll tail) -- out of scope for this OpenController work and
worth little host-awake. The large win is host-asleep, where the dongle halts between windows
(~1.35 mA, R3b) regardless of probe dwell. `KBD_REST_PROBE_ANSWERS` stays 2 for robustness (a
lost answer still leaves >= 1 so the drop counts as scheduled).

## Gates per rung (ppk2d + dongle diag)

1. Connected idle unchanged: 11.5 mA (done today), reply ratio 99.8 %, reconnect 10/10, fresh pair.
2. Rest engaged, host awake: **MET -- 5.96 mA sustained (182 s), zero detector churn, 60/60 probes
   caught. Better than the production keyboard's 6.54 mA on the same path.** (History: 5.74 mA was
   too short a window; a later 8.47 mA figure was an artefact of the Nucleo stand-in poking the
   controller out of rest -- see the correction below.) Earlier text kept for the record:
   5.74 mA is only the current during a *locked/windowed* stretch. The honest sustained average
   over 150 s spanning the detector churn is **8.47 mA** (p50 10.8): the dongle sat unwindowed at
   ~10.85 mA for ~80 s, then settled to ~5.7 mA once locked. Against 11.5 mA connected the real
   host-awake saving is therefore ~3 mA, not ~5.8 mA. The churn (see the probe diagnosis below) is
   the dominant limiter, not a minor optimisation. Re-measure over >= 150 s, never a short window.
3. Rest, host asleep: **PASS (bench 2026-09-11). Clean sustained current 2.593 mA (120 s), halt
   floor 0.197 mA -- against ~10.7 mA for a held link with the host asleep, i.e. ~4x.** Mac slept 17:14:37, woke via `USB2_wake` at
   17:17:12 (~160 s after the Nucleo reset, matching its one-shot autonomous key) while the
   controller rested; `bench_autokey_count=1`. Dongle halted throughout: `halts=515, halted 68.2 s,
   avg 132 ms, abandoned=0`, `usb_suspend_episodes +2` -- the R3b halt engages with OUR resting
   keyboard, every halt returned cleanly, and a keystroke from rest wakes the sleeping host.
   Absolute halting current is a separate host-awake proxy (PPK2 can't sample during Mac sleep):
   pull the dongle USB data cable to suspend it while the Mac stays awake (`rest_halt_current.sh`).
   **Measured 2026-09-11: 2.5 mA mean over 30 s, halt floor 0.196 mA between windows -- but that
   30 s fell in a LOCKED stretch.** The same detector churn applies asleep, so the honest sustained
   asleep average is higher and remains unmeasured; treat 2.5 mA as the best case, not the figure. The halt is deep and
   correct; the 2.5 mA (vs production R3b 1.35 mA) is the probe: a ~121 ms/s dongle-connect burst at
   ~11 mA (dongle catches our probe, promotes, holds ~120 ms) dominates against the 0.196 mA floor.
   **Host-asleep is where probe dwell matters** (host-awake it was a small delta on a 4 mA floor;
   asleep it is ~1.3 mA of a 2.5 mA total). Open optimisation: shorten the dongle's post-probe hold
   -- understand the 121 ms (dongle supervision is only 18.8 ms) before trading answer count for
   lock robustness. The data-cable-pull is a host-awake proxy for fast iteration (no Mac sleep).

   **Probe-optimisation attempt (2026-09-11, PPK2 = the trusted meter now; R3b's 1.35 mA was a
   manual inline meter).** `KBD_REST_PROBE_ANSWERS` 2 -> 1 gives an identical 2.55 mA and identical
   ~123 ms probe burst -- answer count is NOT the lever. The 123 ms burst per probe (dongle radio-on
   at ~9.6 mA, once/second) is the whole gap to the 0.196 mA halt floor, and it is dongle-side: the
   dongle catches our bonded-reconnect probe, promotes, and stays radio-on ~123 ms before halting
   resumes, regardless of how the keyboard probes. `rf_win_on_promote` does not clear `rf_win_mode`
   and the halt gate stays armed, so it is not a windowing re-lock; the exact cause needs the
   dongle's live view (out of scope here). **Conclusion: the OpenController resting policy is
   complete; lowering 2.55 mA further is an OpenDongle change (shorten the radio-on time when it
   catches a scheduled resting probe).** Open question before any dongle work: measure the
   production keyboard resting on the PPK2 -- if it also reads ~2.5 mA, there is no gap and 1.35 mA
   was meter error; if ~1.3 mA, production's probe avoids the burst and that is the thing to study.

   **Root cause found, optimisation deferred (2026-09-11).** With the dongle plugged back in, its
   detector is visible live and it CHURNS: `locks`/`unlocks` climb ~1/s and `unscheduled: rx_low`
   climbs steadily. `rx_low` = a drop the dongle saw with `rf_win_link_rx < 1`, i.e. it received
   ZERO of our probe's response polls. A drop counts as "scheduled" -- keeping the detector locked
   so the reacquire stays windowed/halting -- only with `link_rx >= 1`; on an `rx_low` drop the
   detector unlocks and runs a CONTINUOUS reacquire scan, which IS the ~123 ms radio-on burst. So a
   third to a half of our probes land no counted response on the dongle, and those bursts are the
   whole gap between 2.5 mA and the 0.196 mA floor. It is NOT an answer-count issue: `rest_answers`
   counts our TX, not the dongle's RX. `ANSWERS` 1 / 2 / 6 and an 80 ms post-catch dwell all still
   churn (6 and the dwell were similar or slightly worse). The probe's brief bonded reconnect does
   not reliably deliver a response the dongle actually receives -- an RF hop-phase reliability
   problem in the reconnect itself (same class as the original bonded-reconnect timing work),
   needing focused RF effort and possibly dongle-side cooperation, not an OpenController teardown
   knob. **Deferred as a follow-up; the resting policy ships at 2.5 mA asleep, functionally
   complete.** Code reverted to the simple answer-count probe (`ANSWERS=2`).

   **Production control attempt (2026-09-11): BLOCKED on bench plumbing, not measured.** To learn
   whether the detector churn is our bug or inherent, the plan was to measure a production keyboard
   resting on the PPK2. Stock firmwareB was flashed to the controller (whole-chip erase, dongle bond
   cleared, both ends unbonded) but it could not be paired: the probe UART `stock_link.py` drives is
   no longer wired to the controller (those wires now go to the Nucleo stand-in), and driving the
   stock module from the Nucleo needs its UART wake handshake -- a `0x00` wake byte + 300 ms was
   added to the bench keymap (`bench_cmd 6`) and both `{0x30,0x52,0x51}` (OpenController) and
   `{0x30,0x63}` (production, per `stock_link.py`) were tried; frames are ACKed but no pair. To run
   this control, rewire the probe UART to the controller so the proven stock harness works.
   OpenController was restored and re-paired; bench verified resting at ~5.96 mA windowed.

   **PRODUCTION CONTROL RESULT (2026-09-11, after rewiring the probe UART -- TX/RX had been
   swapped; correct is controller PB13=TX -> probe RX, PB12=RX <- probe TX).** Stock firmwareB
   paired and rested; dongle windowed. Sustained PPK2 comparison, host awake:

   | | sustained resting | detector `rx_low` churn |
   |---|---|---|
   | production keyboard | **6.54 mA** (183 s) | +11 / 183 s = **0.06/s** |
   | our OpenController  | **8.47 mA** (150 s) | **0.35-0.8/s** |
   | windowed floor (both) | ~5.96-6.0 mA | -- |

   **The churn is OURS, not inherent.** Production also churns, but 6-13x less often (one bad probe
   in ~16 vs one in ~1.3-3 for us), and both reach an identical windowed floor -- so the dongle's
   windowing is fine and the whole ~1.9 mA gap is our probe failing to land a counted response.
   This justifies the probe-reliability work and gives it a concrete target: drive `rx_low` down to
   ~0.06/s. (The asleep control was not run -- it needs another USB-data-cable pull -- but the gap
   there should be proportionally larger, since the asleep floor is 0.196 mA so churn dominates
   more; our 2.5 mA asleep figure was itself a best-case locked stretch.)

   **CORRECTION (2026-09-11, and it reverses the churn conclusion).** Measured on the SAME probe-UART
   path the production control used -- i.e. with a QUIET UART master -- our resting policy shows:

   | setup | sustained resting | `rx_low` churn | probes caught |
   |---|---|---|---|
   | **ours, quiet UART master** | **5.96 mA** (182 s) | **0** | **60/60, 0 phase misses** |
   | production keyboard | 6.54 mA (183 s) | +11 (0.06/s) | -- |
   | ours, Nucleo stand-in driving | 8.47 mA (150 s) | 0.35-0.8/s | -- |

   Link verified alive during the clean run: catches +60 and phase_opens +60 per 60 s, promotes
   1.0/s. **So the resting policy is NOT defective -- it beats production (5.96 vs 6.54 mA) with
   zero detector churn.** The churn was the Nucleo stand-in: its bench auto-pair fires every 8 s
   whenever QMK believes the link is down, and during a silent rest QMK's view does go stale, so it
   repeatedly poked the controller out of rest. The earlier 8.47 mA figure is therefore an artefact
   of the stand-in, and 5.74 mA was simply too short a window.

   **Real-product caveat, still open.** The stand-in's auto-pair is bench-only, but the underlying
   issue is not: QMK's `sync_target()` calls `reconnect_2g4()` whenever `selected_target` reads
   UNKNOWN, so a keyboard MCU that reacts to the controller's silent rest WILL disrupt it. Design
   decision D4 (rest is transparent to the MCU) therefore needs the MCU-side driver to hold its
   link view across a silent rest. Verify on real keyboard firmware before shipping.

   **CLEAN ASLEEP RE-MEASURE (2026-09-11, quiet UART master, 120 s):** **2.593 mA**, halt floor
   0.197 mA (p05/p50 both 0.198). Essentially identical to the earlier 2.55 mA, so unlike the awake
   figure the asleep one was already clean -- that 30 s window happened to land in a locked stretch.
   Composition per second from the raw trace: five ~34.6 ms dongle camp windows (~173 ms) plus our
   ~125 ms probe burst = ~30 % radio-on against the 0.197 mA floor. The remaining levers are both
   dongle-side: its camp window schedule (`PM_RX_PERIOD_MS` 200 / `PM_RX_WINDOW_MS` 30, which trades
   first-key latency) and its ~125 ms radio-on reaction to catching a probe. R3b's 1.35 mA was a
   manual inline meter; it is not reproducible on the PPK2 and should be treated as meter error.
4. First key from rest: latency <= 220 ms (mark on the tap, HID tickle on the host), no lost key.
5. Controller-side current between probes (after D7): deep sleep confirmed by the PPK2 trace.
