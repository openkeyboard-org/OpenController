# Controller-side refutation: the key-down reaches the air (clean, boot-verified)

Bench: CF148=controller (instrumented), CEBD=dongle (PPK2-powered). Driver = Nucleo running
handwired/opencontroller_bench over the controller UART. Test key = virtual key 0 (F24), 60 ms tap.

## Counters (rf_task.c, plain .bss; read via SWD post-run OR the UART DiagDump v3)
- ll_hid_rx / ll_hid_tx        : all HID reports queued from UART / all LEN-10 frames selected
- ll_hid_rx_down / ll_hid_tx_down : non-zero (key-down) only: reached RF_QueueHIDReport / selected in rf_do_response_tx
- ll_hid_tx_done_down          : key-down frames that COMPLETED on air (TX_MODE_TX_FINISH), not merely selected

## Measurement hazards discovered
- minichlink SWD reads are NON-destructive (boot_count stable across reads) BUT reading during/near
  an RF op faults the CH592 -> WWDG reboot (BOOT.md "TMOS misaligned-load fault under halt/read").
- The controller reboots ~once per run via WWDG-through-deep-sleep on the first rest, zeroing the
  .bss counters. ll_boot_count (.diag_safe, retained) detects it. Clean-run gate: rx_down==N AND the
  zero-selection invariant (zero_selected <= 6*zero_queued) both hold.
- The QMK driver re-arms autosleep continuously (opencontroller.c:429), so A6 56 does not stick.
- bench.py `diag` reads the DiagDump through the Nucleo trace = non-destructive to the controller.

## Clean run (12 first-key-from-rest trials, 8 s rest, zero-baseline via A6 72)
    tx_done_down=48  tx_down=48  rx_down=12  tx=120  rx=29   reboots=1 (pre-trial-1)
    rx_down==N (12==12) CLEAN ; invariant OK (zero_selected=72 <= 102)
    => 12/12 key-downs reached the controller (no UART-wake loss)
    => tx_done_down == tx_down == 48 : EVERY selected down COMPLETED on air (no failed RF_Tx starts,
       no TX_FAIL). Guaranteed on-air completions >= ceil(48/6) = 8 distinct downs of 12.

## Conclusion
Codex hypothesis D (60 ms release overwrites the press in the controller's single hid_report slot
before it is transmitted) is refuted for the aggregate: the down is not only SELECTED but COMPLETES
on air. The guaranteed-on-air minimum is no longer 0 (Codex point B) -- it is >=8/12. The first-key
loss is therefore downstream of the controller TX (RF propagation, or the dongle receive->EP1->USB
path), EXCEPT for any residual minority of trials whose down is never selected (tx_down=0), which
needs per-trial attribution to rule out.

## Next (step 2 continued)
- Dongle: a pre-dedup non-zero HID RX counter to split RF-loss from EP1-loss.
- Host: real per-key delivery oracle (fix F24=111 mislabel -> F12; use F13 CGEvent or raw HID capture).
- Per-trial attribution via the non-destructive DiagDump (single-tap trials).

## End-to-end 3-hop down-frame accounting (dongle side added, Codex-reviewed)

Both firmwares instrumented; counters corrected after Codex review:
- controller: tx_done_down completion gated on response_pending + flag cleared at each response start
  (was: a stale inflight flag could let a later keepalive/beacon TX_FINISH overcount).
- dongle rfd_hid_rx_down: boot-keyboard-EXACT (tag 0xA1, LEN-10, non-zero body) -- the classifier also
  accepts consumer 0xA3/mouse 0xA8 which route to EP3/EP2, not EP1.
- dongle usb_ep1_arms_down / usb_ep1_completions_down: non-zero (key-down) EP1 arm / IN completion.

Clean, boot-verified run (12 first-key-from-rest trials, 60 ms taps; ctrl rx_down≈N, invariant OK):
    controller ON AIR   (tx_done_down)   = 36
    dongle RF RECEIVED  (rfd_hid_rx_down) = 36   -> RF loss = 0
    dongle EP1 ARMED    (ep1_arms_down)  = 36   -> forward loss = 0
    dongle EP1 DELIVERED(ep1_comp_down)  = 33   -> EP1 overwrite loss = 3

=> The downstream loss is ENTIRELY at the dongle EP1 single-slot overwrite (3 key-down frames
   overwritten before the host read them). Controller TX, RF propagation, and RF->USB forward are all
   clean (0 loss). This is exactly the hop the Goal A plan step 2 (dongle EP1 queue) addresses.

Caveats (Codex, still open):
- FRAME counts include resends (~3/down). A down with 2 frames, one overwritten, still delivers if the
  other completes -> "EP1 loss = 3 frames" is NOT "3 keys lost". Per-KEY verdict needs single-tap trials.
- ep1_comp_down attribution via a single inflight flag has a HW-completion/DMA race (can mis-attribute
  by ~1); arms_down-completions_down also includes suspend/reset, not only overwrite (no suspend this run).
- rfd_hid_rx_down is AFTER the dongle quiesce/state gates, so it folds RF loss + admission loss; here
  RF loss = 0 so admission loss = 0 too. A pre-gate PHY-callback counter (hal_rf_ch592.c) would separate them.

## Per-KEY delivery verdict (host oracle) -- the defect does NOT reproduce awake

Codex's step (a): built a trustworthy host-delivery oracle. Raw HID capture (hidapi) is BLOCKED --
macOS refuses to open the seized boot-keyboard interface ("open failed"). Fell back to Codex's
alternative: temporarily reflashed the bench Nucleo with KC_F24->KC_F13 (F13 = HID 0x68, macOS
keycode 105, which the CGEventTap DOES surface; F24 produces no CGEvent). Keymap SOURCE reverted to
F24 (repo clean); the Nucleo currently runs the local F13 build for oracle testing.

12 first-key-from-rest trials (rest 8 s -> "waiting for reconnect", tap 0 = F13 60 ms via the driver,
CGEventTap capturing keycode 105):
    RESULT: 12/12 first keys DELIVERED (host saw keyDown 105 + keyUp 105, ~60 ms apart, every trial).

=> On this bench (host AWAKE, EP1 polled every 1 ms) the first-key-from-rest defect does NOT reproduce
   as key loss. The host reads the key-down within ~1 ms of EP1 arming, long before the 60 ms-later up
   arms, so the EP1 single-slot overwrites measured earlier are of REDUNDANT RESEND frames, not the
   down-vs-up race. The earlier "1/6 delivered" was the contaminated HID idle-timer oracle.

=> Consistent with Codex: the report-delivery loss requires a condition where the host is NOT draining
   EP1 -- most likely actual USB SUSPEND/WAKE (down arms while suspended, up overwrites the single
   slot, resume delivers only the up / a stale report). That is the failing case to reproduce next,
   and it is exactly what the Goal A plan step-2 gate ("continuous typing THROUGH a wake") targets.

## CORRECTION: host oracle confounded by the Nucleo's parallel USB
The bench Nucleo's own user USB enumerates on the Mac as keyboard "OpenController Bench" (VID 0x1209),
separate from the dongle "OpenDongle 2.4G" (0x0C45), and delivers the tapped key IN PARALLEL with the
module->dongle path. Proven: in OC_USB mode (dongle not the path) an F13 tap STILL reached the host.
So the CGEventTap "12/12 delivered" is NOT a dongle-path result. The valid dongle-path oracle is the
dongle's own counters. Suspend tests v1/v2 were invalid (tap-disabled-across-sleep + DarkWake; and a
reflash that broke the 2.4G bond -> USB fallback, rfd_hid_rx_down=0). 2.4G link recovered via OC_2G4.

## THE DEFECT REPRODUCES (once the dongle is actually the delivery path)

Root cause of all earlier confusion: the bench driver's default host is AUTO, and `desired_target()`
(qmk mk65mx_wireless/opencontroller.c:115) resolves AUTO to **OC_TARGET_USB whenever the Nucleo's own
user USB is plugged into the host**. Every Nucleo reflash reset the host to AUTO, so the taps went out
the Nucleo's own HID (VID 0x1209) and NEVER traversed the module->dongle path. Fixed by forcing
CONNECTION_HOST_BLUETOOTH at startup in bench.c (BLUETOOTH always resolves to OC_TARGET_2G4, no USB
fallback), making the dongle the only keyboard that can deliver.

With the dongle as the real path, first-key-from-rest (60 ms tap, 8 s rest), host oracle = CGEventTap
on F13 (now unambiguous):
    run 1: 8/12 delivered (4 LOST)
    run 2: 7/12 delivered (5 LOST)
=> ~35-40% of first keys from rest are LOST. This is the failing baseline the plan/Codex required.

### Per-trial localization: the lost DOWN never reaches the dongle
    LOST trials      -> down-frames reaching the dongle = 0
    DELIVERED trials -> down-frames reaching the dongle = 2..6
Aggregate over 12 taps: controller on-air down-frames (tx_done_down) = 108, dongle received
(rfd_hid_rx_down) = 40, EP1 armed = 40, EP1 completed = 36. So the controller transmits the down
MANY times but most on-air frames land while the link is not yet re-established, and for the lost
trials NONE arrive.

### Mechanism (hypothesis D, realized at the reconnect boundary)
The tap arrives while the link is rested. The controller queues the down into its single `hid_report`
slot and transmits it on air repeatedly DURING the reconnect window -- but the dongle is not connected
yet, so those frames are lost. 60 ms later the key-UP overwrites the same single slot. When the link
finally comes up, only the UP is transmitted. The key-down is gone.

My earlier "refutation" of hypothesis D was too narrow: tx_done_down proved the down reached the AIR,
but on-air != delivered. Codex's warning that a completed TX still does not establish dongle receipt
was exactly right.

### Consequence for the fix
The dominant first-key loss is at the CONTROLLER (single slot + no retain-until-acknowledged across a
reconnect) = Goal A plan **step 3** (controller RF FIFO + sent-head retire), NOT the dongle EP1 queue
(step 2). The dongle EP1 path is healthy here: it arms and completes essentially everything it
receives. Step 2 remains a correctness improvement but is not the cure for this defect.

## (B) Suspend/wake: NO dongle-side loss (stash path is not the defect)
Two runs (tap fired 25 s and 60 s into macOS sleep, bench correctly routed via the dongle):
    stash_down=0 stash_clobber_down=0 stash_deliver_down=0   (suspend-stash NEVER entered)
    rf_down=12  ep1_arms_down=12  ep1_comp_down=10..11  ep1_ov=2
    host CGEvents = 0
The dongle received the waking key-down, armed EP1 and the host's USB stack COMPLETED it; the 2
uncompleted frames are redundant resend copies. The suspend stash is never reached because the
dongle's remote-wake resumes the bus before the report frames arrive (wake log: DriverReason
USB2_wake). The absence of a user-visible key event is macOS DarkWake (reports consumed by the
driver, not surfaced to the session), not a firmware loss.
=> The single-slot suspend stash is NOT the first-key defect. The defect is the controller-side slot
   loss at the reconnect boundary (see above). Proceeding to plan step 3.

## STEP 3 IMPLEMENTED: ack-retired report FIFO on the controller

Replaced the single `hid_report` slot + blind `hid_resend` attempt counter with a 16-slot
single-producer/single-consumer ring (stock's module keeps 20):
- enqueue in RF_QueueHIDReport (main loop), deduped only against the NEWEST QUEUED state so
  down -> up -> same-down is preserved; a full ring drops the newest (best-effort, as stock).
- the head is re-sent on every poll and retired ONLY when the dongle advances its control bit
  (rf_task.c ack site) -- `hid_head_sent` guards an advance that precedes our first send.
- rest gate now requires an EMPTY fifo (was: resend counter zero).

### Result (same recipe, same oracle, dongle-routed)
    BEFORE (single slot):  8/12 and 7/12 delivered   = 15/24  (62 %)
    AFTER  (ack FIFO):    12/12 and 10/12 delivered  = 22/24  (92 %)
Counters after a clean 12-trial run: fifo_drop=0, rx_down=24, tx_down=24, tx_done_down=24
-> every enqueued key-down is put on air and acknowledged; no blind resends, no queue overflow.

### Residual
2 of 24 trials still lost, and BOTH had the link reported `connected` at tap time -- i.e. a
different, smaller failure mode than the reconnect-gap loss this step fixed. Needs its own
investigation (candidates: a loss while nominally connected, a status-poll race, or host-side
surfacing). Step 3's own gate (a press survives the reconnect gap) is met.
