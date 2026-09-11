# Production firmware and OpenController power management

Consultant review, 9 September 2026. Static reverse engineering and source review; no firmware, configuration, or hardware changes were made.

Bench addendum, 10 September 2026: the `HalSleepFalse` variant of the same image was run on a bench board against OpenDongle. Observations are marked **Observed** and collected in [Bench observations](#bench-observations-10-september-2026); nothing else in the review was rewritten.

## Assessment

**OpenController already implements substantially the same retained deep-sleep mechanism as production. The largest potential saving is in when the radio is allowed to stop, rather than in a lower-level sleep instruction or a smaller retention mask.** Production has an automatic inactivity ladder that can interrupt an otherwise connected 2.4 GHz session, briefly search for its peer at intervals, and eventually stop searching. OpenController deliberately keeps connected links awake and keeps a bonded reconnect search running until it succeeds or the host changes state.

Four findings materially affect the comparison:

1. Production uses a **low-level GPIO interrupt on UART1 RX/PB12** for host wake. OpenController uses a **falling edge** on the selected UART1 RX pin. A held-low wake signal therefore deserves investigation as a way to address OpenController's documented check-to-sleep race.
2. The production image has **two inactivity stages**, approximately five seconds and thirty minutes, with different purposes. Its persistent `A6 55`–`A6 58` flags control the later stage; they do not gate all MCU sleeping. OpenController's `A6 57` instead authorizes deep sleep in already safe RF states after a 100 ms activity holdoff.
3. **`A6 54` is not an immediate-sleep command in this particular production image.** Its command dispatcher ignores that subcommand, although the parser acknowledges the frame and performs its normal activity handling. OpenController implements an actual immediate-sleep request after capability negotiation. Production `A6 56` disables Bluetooth auto-sleep, whereas OpenController repurposes it to negotiate its sleep protocol.
4. **`A6 63` is not a pairing command in this image** (Observed). The parser acknowledges it and nothing follows; production pairs on `A6 30` then `A6 51`, the path OpenController also implements. OpenController's `0x63` "factory 2.4G pair" handler is an extension, so a host that pairs with it works only with OpenController.

Recommended order: make the host wake contract reliable, ensure compatible hosts actually enable the existing savings, then consider a negotiated inactivity timeout and progressively slower reconnect searches. Preserve the current connected-link sleep veto until a deliberate disconnect or a fully scheduled radio-idle window makes sleeping safe.

## Scope, provenance, and confidence

| Item | Reviewed input |
|---|---|
| Production image | `Dongle-Production/firmwareB.bin` (held locally; not part of this repository) |
| Size | 179,772 bytes (`0x2BE3C`) |
| SHA-256 | `4e319cd5474b8e2e3901eea9eb74fae8538b127b924151761db35a8f9da0ccee` |
| OpenController revision | `40bfb45cacddf8a1ad3a9623de477dca62315bc7` |
| Board profiles | `opencontroller-ch592` and `mk65mx-wireless-ch592`, with their committed defaults |
| Production library identification | Embedded `CH59x_BLE_LIB_V1.0` / `CH59x_BLE_LIB_V1.00` strings; these do not identify a complete SDK release |
| OpenController library selection | Makefile default: OpenWCH CH592 BLE library V1.4.2; peripheral/HAL sources from pinned WeActStudio SDK |

Despite the parent directory's name, the binary contains the keyboard-side UART command parser, HID handling, Bluetooth roles, and proprietary 2.4 GHz keyboard RF state machine. It is therefore a relevant comparison for OpenController, rather than merely USB receiver suspend firmware.

**Confirmed** below means supported by instructions, control flow, register definitions, or current source. Descriptive production function and variable names are reconstructed labels, not recovered symbols. **Inferred** means an interpretation of that evidence, particularly the intended user experience. **Unmeasured** includes current, battery life, wake reliability, and interoperability under the proposed policies. **Observed** means seen on the 10 September bench run described below; it adds no current measurements.

This application image does not contain the device's actual DataFlash configuration, bootloader, host-MCU firmware, or board netlist. Persistent defaults can be reconstructed, but the configuration of a shipped unit and the host's wake waveform cannot be established from this file. No new current measurements, flashing, or debugger attachment were performed. Historical measurements quoted from OpenController comments are identified as such.

### Address reconstruction and method

The adjacent `firmwareB.hex` was checksum-validated and reconstructed: its contiguous address range is `0x00001000`–`0x0002CE3B`, and its payload exactly equals `firmwareB.bin`. Startup at `0x46C4` independently confirms:

| Runtime region | Initialization |
|---|---|
| SRAM `0x20000000`–`0x200036B7` | Copied from flash `0x100C`–`0x46C3`; contains vectors and RAM-executed code |
| SRAM `0x200036B8`–`0x20003E3F` | Initialized data copied from flash `0x2C6B4`; length `0x788` |
| SRAM `0x20003E40`–`0x2000629F` | Zeroed BSS |
| Global pointer / initial stack | `gp = 0x200045A0`; `sp = 0x20006800` |
| Application entry / vector base | `main` at `0x99C0`; vectors at `0x20000000` |

Consequently, for the RAM-code region, **file offset = runtime address − `0x20000000` + `0x0C`**. For flash-resident code, **file offset = flash address − `0x1000`**. Disassembling the entire file at one address would give incorrect targets for its RAM-executed functions.

Analysis used WCH GNU objdump and Ghidra 12.1.3 headless analysis with the startup mappings and global pointer restored. Important decompiler findings were checked against instructions. Apparent functions in the flash load-image copy of RAM code were not treated as additional runtime paths. Scratch disassemblies and the analysis project were kept outside the repository in `/private/tmp/opencontroller-power-review`.

As a secondary cross-check, the nearby `firmwareB-HalSleepFalse.bin` differs in only 11 bytes. The changed instruction groups at file offsets `0x5E56`–`0x5E5D` remove the idle callback pointer, and `0x6044`–`0x6049` make its entry return 3. This corroborates the identification of the sleep callback; it does not substitute for tracing the requested image.

## Production implementation

### Main loop and sleep ownership

The RAM main loop at `0x20001988` repeatedly runs TMOS processing, the proprietary connected-hop update, and the UART parser. TMOS processing is skipped when an update-related flag is set. There is no unconditional application-level idle `WFI` in that loop.

The initializer at `0x6D88`, specifically `0x6E56`–`0x6E68`, passes the function at **`0x7044`** as the BLE/TMOS idle callback. TMOS's idle dispatcher at `0xAF38` combines its software deadline with the radio scheduler's deadline before calling it. A callback result of zero can trigger library radio/baseband restoration. This supports OpenController's decision to perform deep sleep inside the TMOS callback.

The image also contains SDK idle, halt, and shutdown primitives, plus a library-internal shallow `WFI` in `0x20002DEC`. Presence alone does not establish application usage: the traced application power policy calls the retained-sleep primitive at `0x20000298`. The library wait is part of radio operations, not evidence of a general low-current application idle policy.

### Reconstructed state and timing

The application task at **`0x7C54`** owns the inactivity policy. Its relevant state is:

| RAM address | Reconstructed meaning |
|---|---|
| `0x2000467E` | Permit the MCU idle callback to sleep |
| `0x2000467F` | Later/full-sleep mode flag |
| `0x20004680` | First-stage low-power mode flag |
| `0x20004684` | Transport/role selector; 2 selects proprietary 2.4 GHz |
| `0x2000462C` | Proprietary RF state: 0 stopped, 1 searching, 2 connected |
| `0x20005C66` / `0x20005C67` | Persistent Bluetooth / 2.4 GHz later-stage auto-sleep enables |
| `0x20003ED7` | Failed short-search counter |
| `0x20003EAC` | RTC trigger-fired flag |

Timer calls at `0xAB40` convert their arguments using a divisor of 1600 events/second. This agrees with WCH's `SYSTEM_TIME_MICROSEN = 625` definition. These timings are therefore supported by the binary, rather than guessed from familiar constants:

| Event in task `0x7C54` | Delay | Observed purpose |
|---|---|---|
| `0x0010` | 8,000 ticks = **5 s** | Enter the first inactivity stage; armed by `0x7524` |
| `0x0010`, pairing/update paths | 96,000 ticks = **60 s** | Extend the first-stage activity window |
| `0x0020` | 1,600 ticks = **1 s** | Interval between first-stage 2.4 GHz reconnect checks |
| `0x0020` | 16 ticks = **10 ms** | Allow a short reconnect-search attempt before evaluating it |
| `0x0001` | 2,880,000 ticks = **30 min** | Later/full-sleep inactivity timeout, scheduled by `0x786C` when the selected transport's persistent flag is enabled |
| `0x0001` retry | 48 ticks = **30 ms** | Retry while transport shutdown/disconnection completes |
| `0x0004` | Posted by GPIO or qualifying remote activity | Leave inactivity mode and restore UART/radio operation |

The first stage is armed at initialization and on activity **without testing the persistent auto-sleep flags**. Ordinary validated UART frames rearm the later-stage timer and ensure at least five seconds remain before the first stage. Host `61 0D 0A` acknowledgements take an early parser path and do not do this. Certain remote status changes also restart the five-second timer. Thus “five seconds idle” means inactivity as defined by these paths, not necessarily five seconds without any UART byte or RF packet.

#### First inactivity stage: sleep between brief radio attempts

On event `0x0010`, the task sets both the sleep-permitted and first-stage flags. In 2.4 GHz mode:

- If connected, `0x725E(0)` shuts down the RF operation, cancels RF work, and changes the RF state to stopped. The zero argument suppresses that helper's disconnect status message. It then schedules event `0x0020` in one second.
- If already searching, `0x721A` stops the search and the task advances to the later/full-sleep path.
- If already stopped, it can enter the GPIO/RTC-wake configuration without starting a periodic search from this branch.

Event `0x0020` alternates between starting a search from the stopped state and examining it approximately 10 ms later. A successful connection is stopped again for the next one-second interval. A search still unresolved at the check is stopped and increments the failure counter; after the third such failure, the task posts the full-sleep event.

The 2.4 GHz receive callback at `0x20001192` clears sleep permission on a successful first-stage reconnect. Selected remote activity can post the full wake event. **This is repeated stop/search/reconnect behavior, not proof that production preserves a live proprietary link across deep sleep.** Exact receiver-side perception and reconnect success probability require a packet trace.

Observed (10 September, `HalSleepFalse` variant, OpenDongle as receiver): with the link connected and the host silent, the receiver reported the session gone 5.0 s after the last validated frame (frames every 4 s never trigger the stage; every 6 s, the lapse follows each frame by 4.98 to 5.01 s), then stopped and reconnected with a 1.010 s period (standard deviation 15 ms over 15 cycles), each connection lasting on the order of 10 ms. The receiver caught every short search, so the failure counter never advanced and the full-sleep transition was never reached while it was present. The ladder ran unchanged with the sleep callback neutered, consistent with the 11-byte difference between the two images: the inactivity policy does not depend on the retained-sleep primitive.

Before waiting, the application parks UART TX/PB13 as a pulled-up input, configures UART RX/PB12 for low-level GPIO interrupts, enables GPIO-B wake, and uses the long wake delay. The TX send routine makes PB13 an output again when transmitting.

#### Later/full-sleep stage

Event `0x0001` sets the full-sleep flag, cancels the first-stage and pending wake events, and tears down active 2.4 GHz work. Where necessary it retries transport teardown after 30 ms before reaching the GPIO-wake setup. The 30-minute inactivity timer is one route to this event; failed short searches and other first-stage transitions are additional routes.

This stage still uses **retained sleep**, with TMOS/RTC deadlines and housekeeping wakes. It does not call the reset-on-wake shutdown primitive. Battery-report event `0x0002` continues to reschedule every five seconds but skips its battery-read/report call while the full-sleep flag is set. Therefore this is not necessarily a single uninterrupted thirty-minute or indefinite hardware sleep.

#### Bluetooth scope

Production also has a functioning BLE role implementation. The first stage checks `GAPROLE_STATE` (`0x30D`) and leaves an established BLE connection (`4`) intact, allowing the stack to schedule sleep between BLE events. The later stage requests BLE termination. This differs from the proprietary 2.4 GHz policy above. OpenController currently has no working BLE HID transport, so BLE connected-sleep savings are an additional feature gap, not an immediately portable optimization of its present RF link.

### Sleep entry and return

The callback at **`0x7044`** is approximately the following. Names are reconstructed and interrupt-save details abbreviated:

```c
if (!sleep_permitted)
    return 0;                       // actual production behavior
if (UART1_TX_FIFO_COUNT != 0)
    return 3;

disable_UART1_irq();
deadline = rtc_subtract_with_wrap(deadline, 51);
saved = save_and_disable_peripheral_irqs();
remaining = rtc_distance(now(), deadline);
if (remaining < 32 || remaining > RTC_MODULUS - 30000000) {
    restore_peripheral_irqs(saved);
    return 2;
}
set_rtc_trigger(deadline);
restore_peripheral_irqs(saved);       // UART1 was excluded before the save
if (rtc_trigger_fired || !PB12_is_high())
    return 3;

retained_sleep(0x401A);
clear_PA14_output();
set_HSE_bias_to_100_percent();
wait_for_one_RTC_counter_change();
return 0;
```

The RTC runs from the internal approximately 32 kHz source: initialization programs the clock choice and supplies a 32,000 Hz timebase to TMOS. The callback wakes **51 RTC ticks early**, approximately **1.594 ms**, and rejects remaining windows shorter than **32 ticks**, approximately **1 ms**. Its modulus is `0xA8C00000`; the large maximum is a wrap/deadline guard, not a realistic normal sleep target. Housekeeping timers normally produce much shorter sleeps.

`HAL_SleepInit` equivalent at `0x7120` enables the RTC wake source and trigger mode globally. The ISR at `0x20000C2E` acknowledges RTC flags and marks the trigger as fired. The application does not disarm trigger mode after each sleep in the way OpenController does.

The primitive at **`0x20000298`**:

- Disables battery detection and adjusts oscillator bias; raises HSE bias to 150% for wake reliability.
- Sets `SLEEPDEEP`, low-voltage SRAM retention, and a power plan preserving regulator configuration.
- Receives `0x401A`: retain 2 KiB SRAM, 24 KiB SRAM, and the extended peripheral state, with early HSE startup. It additionally retains the core through the power-plan bits.
- Clears WFI-to-WFE mode and executes plain `WFI` at **`0x200004B4`**, then performs the post-wake register sequence.

The wrapper restores HSE bias to 100%, as in WCH's published [CH592 power-management example](https://github.com/openwch/ch592/blob/main/EVT/EXAM/PM/src/Main.c). Both the primitive and wrapper must be considered when comparing power after wake.

### UART wake and entry checks

UART initialization at `0x9668` explicitly enables the UART1 remap and configures PB12/PB13 at 115,200 baud. **PB12 is UART RX itself, not a separate wake wire.** Low-power entry calls the GPIO configuration helper at `0x55A0` with `(0x1000, 0)`. The helper clears PB12's edge-mode bit and selects low polarity: **low-level interrupt**. This is confirmed by register writes, as well as the SDK enum ordering.

GPIO-B ISR **`0x20001818`** clears PB12's flag, clears sleep permission, and posts application wake event `0x0004`. The task then disables the PB12 GPIO interrupt, restores the UART1 interrupt, clears inactivity flags, and restores the selected radio mode. A 2.4 GHz wake can restart the bonded search automatically; one path gives it a 200 ms fast attempt before falling back to its normal search behavior.

The binary does not establish the host's preamble duration or whether the waking byte survives. With the UART IRQ disabled and the high-speed domain asleep, treating that byte as expendable remains prudent. A low-level wake can remain asserted through entry **if the host holds the line low**; a short byte that finishes before the final wait can still fail to provide that guarantee. Level-triggering is useful evidence for a better handshake, not proof of a race-free production protocol.

Production checks the **TX FIFO count**, rather than explicitly checking the UART shift register's all-empty indication. It also checks PB12 high and the RTC-fired flag, but the callback has no OpenController-style software-parser, RX-ring, pending-GPIO, or bootloader-latch checks. UART IRQ disable can remain in effect after a callback veto until the application wake path restores it. These are differences to account for, not behaviors to copy uncritically.

Observed: after the first stage had been entered, a bare `A6 63 09` frame drew no reply, consistent with its first byte being consumed as the wake. A `0x00` byte, a 300 ms gap, then the frame was answered every time; shorter gaps were not tried. The module itself emits `61 0D 0A` after every valid frame it accepts, so that sequence runs in both directions, and a keepalive (an all-keys-up `A1` report every 2 s) held the module in its active state indefinitely.

### Command semantics and persistence

All command bytes below omit the additive checksum in the table. For example, `A6 54` is transmitted as `A6 54 FA`.

| Command | This production image | OpenController default deep-sleep build |
|---|---|---|
| `A6 54` | No dedicated command action. Generic parser ACK/activity effects still occur. | Request explicit sleep after `A6 56` negotiation; drain TX, flush bond, disconnect RF, then sleep through TMOS. |
| `A6 55` | Set persistent BLE later-stage auto-sleep enable. | Reserved, ACK-only. |
| `A6 56` | Clear persistent BLE later-stage auto-sleep enable. | Negotiate/reset sleep protocol v1; reply `5B 37 92`; clear reducer pending sleep and disable auto-sleep. |
| `A6 57` | Set persistent 2.4 GHz later-stage auto-sleep enable. | Enable autonomous deep sleep after negotiation. |
| `A6 58` | Clear persistent 2.4 GHz later-stage auto-sleep enable. | Unrecognized/inert apart from ACK/activity handling; does not disable auto-sleep. |
| `A6 11` | Observed: replies `5B 34` (three times). | Select USB: disconnect RF, reply `5B 34`, `5B 36`. |
| `A6 30` | Observed: replies `5B 34` then `5B 36` with no bond, or `5B 32` then `5B 23` after a bonded reconnect, each three times; the reconnect itself completed within the second. | Select 2.4 GHz: reply `5B 34`, then `5B 35` (bonded) or `5B 36`. |
| `A6 51` | Observed: after `A6 30`, replies `5B 31` (three times) and starts pairing; a camping OpenDongle connected within the same second. | Pair the selected transport after the identity check: `5B 31`, `5B 23`. |
| `A6 63` | Observed: generic `61 0D 0A` acknowledgement, no action. | Factory 2.4 GHz pair: disconnect, clear bond, enter pairing. An OpenController extension. |

Production repeats each status frame three times where OpenController sends it once (Observed); a host that counts status frames rather than reading the latest will see the difference. Production's command dispatcher is **`0x8346`**. For `0x54`, the comparisons at `0x83CE`–`0x83EE` fall through to the return at `0x8430`: this is affirmative control-flow evidence of no dedicated action, rather than merely an unsuccessful symbol search.

Production writes both flag changes through **`0x9A40`**, which checksums and saves the configuration block to DataFlash offset `0x6000` (physical `0x76000`). Configuration loading/default construction at `0x9B04` supplies enabled defaults for both flags when it cannot recover a valid configuration. The actual device's flags remain unknown because this application file does not include those DataFlash addresses.

There is a production sequencing nuance: the generic parser updates the later-stage timer **before** dispatching the command. `A6 55` updates it again after changing its flag; `A6 56`, `57`, and `58` save their flag and return without doing so. Their new setting is persistent, but its effect on an already scheduled timer may wait for subsequent activity. Also, disabling either later-stage flag does **not** disable the independently armed five-second stage.

## OpenController implementation

The implementation, rather than older comments describing earlier development stages, is the basis of this section.

### Committed defaults and shallow idle

Both [board profiles](firmware/boards/) enable `KBD_DCDC_ENABLE=1` and `KBD_DEEP_SLEEP=1`. The [Makefile](firmware/Makefile) supplies `HAL_SLEEP=TRUE` and substitutes the application-owned [power_sleep.c](firmware/src/power_sleep.c) for SDK `HAL/SLEEP.c`. `KBD_IDLE_WFI` defaults to 1 in [keyboard_uart.h](firmware/src/keyboard_uart.h). The watchdog and RF diagnostic counters are also enabled by default.

[main.c](firmware/src/main.c) enables the board-supported DC-DC converter before selecting 60 MHz. It parks unused pins, preserving the MK65MX PB13 host-driven CHWAKE input. It sets sleep clock-gating bits for TMR1/2, UART0/2/3, SPI0, PWM, USB, I2C, and LCD; TMR0, TMR3, UART1, and BLE remain available as required.

When RF is `IDLE`, UART parsing is quiet, and no update or explicit sleep needs servicing, the main loop enters **shallow WFE**. It runs the final checks with interrupts masked, uses SEVONPEND and a prior event-latch drain, powers down flash until the next fetch, and leaves `SLEEPDEEP` clear. UART RX interrupts wake it promptly. A TMR3 heartbeat every **200 ms** permits watchdog feeding during long inactivity. Neither pairing nor connected operation takes this application-level shallow wait.

This already improves the idle case that production's application main loop otherwise polls. The name `KBD_IDLE_WFI` is historical: this path deliberately uses the core's WFI-to-WFE conversion.

### Explicit sleep and autonomous sleep

[sleep_protocol.c](firmware/src/sleep_protocol.c) resets negotiation and auto-sleep at boot. Deep-sleep capability being compiled in is therefore **not the same as deep sleep being enabled at runtime**.

For explicit sleep, the host negotiates with `A6 56 FC`, waits for `5B 37 92`, and sends `A6 54 FA`. `Power_Service` waits for TX FIFO and shift-register completion, with an approximately 20 ms timeout, flushes a pending bond save, disconnects RF, and arms `PowerSleep_RequestExplicit`. The next acceptable TMOS idle callback performs the sleep. RTC housekeeping wakes leave the explicit request armed; a GPIO host wake clears it. A GPIO wake alone leaves RF stopped: the host must select/reconnect the transport, normally with `A6 30`.

After negotiation, `A6 57 FD` enables autonomous sleep. It permits deep sleep in `RF_STATE_IDLE` or in a bonded-search slice after RX has closed. Connected operation, fresh pairing, active RX, UART work, and pending OpenBoot entry veto sleep. The **100 ms holdoff** is restarted by accepted frames, consumed raw UART-byte activity, and GPIO wakes. The holdoff is a minimum eligibility delay; main-loop/heartbeat scheduling can extend actual time awake.

Auto-sleep survives transport selection, but pairing/unpairing (`A6 51`, `52`, `63`), renegotiation, and reboot clear it. A newly booted bonded controller therefore searches without autonomous deep sleep until a compatible host negotiates and arms it. Merely sending production's `A6 57` is insufficient.

### Retained deep sleep and wake

`pwr_sleep_until` uses the same nominal **51-tick early-wake margin**, **32-tick minimum**, **`0x401A` retention selection**, RTC timebase, and plain deep `WFI` as production. It uses the pinned SDK primitive, then restores HSE bias to 100%. It arms and disarms RTC trigger mode and GPIO wake around each sleep, checks hardware pending state and RX activity at the boundary, and refreshes the watchdog before entry.

The RX wake pin is **PB12** for `opencontroller-ch592` and **PA8** for `mk65mx-wireless-ch592`. Both use a **falling edge**, with long hardware wake delay. The current `make update` helper sends one NULL byte and a 5 ms gap before its command. Source comments explicitly acknowledge that an edge serviced during the unmasked deep-sleep prologue can be lost before `WFI`; one NULL plus a gap does not formally close that race.

OpenController reports zero to TMOS after every actual deep-sleep entry/return, including an unexpected return, so the library can restore its radio state. It reports a veto/non-entry instead when sleeping is not allowed. Production's callback, unusually, returns zero even when its sleep-permitted flag is clear. That distinction should not be replicated as an optimization.

The low-level routines are **similar, not byte-identical**. Production conditionally handles PLL bit 5 according to the early-HSE option; the pinned OpenController SDK handles it unconditionally and also calls `GetMACAddress` in its sleep primitive. Production waits for an RTC tick after restoring HSE bias; OpenController's wrapper does not. These reflect different SDK implementations and are not evidence that removing either sequence is safe.

### Reconnect search and connected operation

[rf_task.c](firmware/src/rf_task.c) sends bonded-search beacons every **20 ms**. The default receive window is **6 TMOS ticks = 3.75 ms**, beginning when RX actually arms. It then calls `RF_Shut`. Nominal RX-window duty is 18.75%; beacon airtime, entry/exit overhead, and wake margin mean this is not the total awake duty cycle. With negotiated auto-sleep, the radio-off portion can also become MCU deep sleep.

Bonded search is unbounded, while explicit fresh pairing uses continuous RX for its bounded window of approximately 5.3 seconds. The source records that a 2.5 ms search window failed cold reconnect tests and 3.75 ms worked. Reducing the window below six ticks is therefore not a supported shortcut.

Connected hopping is polled by `RF_ConnectedTick` from RTC time, commonly on a **28-tick / approximately 875 µs grid**. Its deadlines are not visible to TMOS. A stock-style idle callback must not sleep across these deadlines. This is also shorter than the current approximately 2.6 ms combined early-wake/minimum-window requirement for retained sleep. Inactivity-based disconnect is a plausible saving; deep sleep between every current connected hop is a separate architectural problem.

## Differences that matter

| Area | Production | OpenController | Consequence |
|---|---|---|---|
| Retained sleep floor mechanism | 26 KiB RAM + extended state, early HSE, plain deep WFI | Same retention selection and sleep class | No demonstrated missing lower-power retention mode in OpenController. |
| Active clock/regulator | 60 MHz; DC-DC enabled when PB4 strap reads low and silicon permits | 60 MHz; DC-DC selected explicitly by board profile | Existing hardware optimization; do not infer production LDO use or claim a new DC-DC opportunity. |
| Application shallow idle | Polling main loop; SDK/library wait routines also present | Deliberate flash-off WFE in RF IDLE | OpenController already has a useful idle optimization. |
| Runtime sleep authorization | Five-second stage automatic; persistent flags govern later timer | Boot-scoped negotiation; auto-sleep initially off | Host integration determines whether OpenController ever realizes its deep-sleep savings. |
| No-key connected 2.4 GHz | Can stop session after inactivity and probe at intervals | Remains connected and awake | Main policy opportunity, with first-key/receiver-state tradeoffs. |
| Absent peer | Short first-stage attempts can end in full sleep | Bonded search continues indefinitely | A progressive search policy can reduce long unattended drain. |
| Host wake | RX low-level GPIO, UART IRQ disabled; TX parked as input | RX falling-edge GPIO; UART/parser guards and activity holdoff | Investigate a held-level wake protocol while preserving OpenController's stronger entry checks. |
| Return from host wake | Application task can reconnect automatically | Explicit sleep leaves RF IDLE; host orchestrates reconnect | Host sequencing must include reconnect and safe delivery of the first report. |
| Pending TX | FIFO count check | FIFO plus shift-register empty, bounded explicit drain | OpenController's guard is more complete. |
| RTC wake | Trigger mode enabled globally; post-wake tick wait | Trigger armed per sleep and disarmed after; no tick wait | Preserve deadline/wrap safety; measure before changing margins or SDK sequencing. |
| Inactive timers | Five-second reporting timer and 120-second calibration work remain | 200 ms shallow heartbeat; TMOS housekeeping during deep sleep | Neither implementation guarantees an indefinitely uninterrupted sleep. |
| Diagnostics/watchdog | No application watchdog-feed loop identified | Default diagnostics and watchdog, with retained crash counters | Diagnostic reduction is secondary; keep watchdog recovery. |
| Bluetooth | Stack-managed connected sleep available | BLE HID not implemented | Do not attribute all production battery-life advantages to its 2.4 GHz implementation. |
| Pairing command | `A6 30` then `A6 51`; `A6 63` acknowledged, no action (Observed) | Same `0x30`/`0x51` path plus a `0x63` factory-pair extension | Hosts and bench tools that pair with `A6 63` work only against OpenController. |
| Receiver's view of the idle ladder | One session stop and one reconnect per second while idle; every 10 ms search caught by OpenDongle (Observed) | Link stays up | A receiver's lapse and reacquire counters read a steady 1 Hz cycle against an idle production keyboard; that is the policy, not a link fault. |

## Potential improvements, in priority order

### 1. Make wake reliable and use the existing sleep capability

**Priority: high; prerequisite for broader autonomous sleep.** Verify that the keyboard host negotiates `A6 56`, recognizes `5B 37`, sends `A6 57`, and renews that setup after every module reboot. Audit its report cadence: traffic more frequent than the holdoff can prevent autonomous deep sleep. Keep the gate until that host supports the wake contract.

Use production's low-level RX configuration as a starting point for a **held BREAK or dedicated held wake request**, followed by a defined release/settling/ready sequence. A repeated expendable preamble with retry is another option. Account for line-status errors, parser cleanup, repeated level interrupts, and when the ISR disables/rearms the wake source. Simply replacing the edge enum is insufficient.

The MK65MX profile documents an already connected host-driven PB13 CHWAKE line, currently left floating by OpenController. A dedicated wake input may avoid sacrificing a UART byte, but its board routing, polarity, and host behavior must be verified; production's PB12 behavior is not evidence about that board-specific wire.

**Benefit:** access to existing deep-sleep savings without a policy rewrite, and reliable first-command delivery. Validate random wake phase, held-low duration, repeated bursts, RTC/GPIO coincidence, and bootloader entry from sleep before shortening any wake gap.

### 2. Add an explicitly negotiated no-key inactivity policy

**Priority: high for keyboards spending hours connected but untouched.** Introduce a configurable idle timeout that waits until keys are released and the final report is delivered, finishes pending traffic, preserves the bond, deliberately disconnects, and requests retained sleep. Sleeping with keys held would need a separate continuity design. Production's five-second first stage demonstrates the policy concept, but that exact timeout is not a recommendation without usability measurements.

On wake, restore the transport and queue the first changed HID report until reconnection is confirmed. Define what happens for held keys, key release immediately before sleep, host LED updates, and peer reconnect state. OpenController currently expects host-paced reconnect after disconnect because immediate firmware-driven retry has previously caused a livelock; preserve that coordination in a new policy.

**Benefit:** potentially large reduction in average radio/CPU activity while the user is absent. **Cost:** reconnect delay and risk of losing the first key event if the host and controller are not designed together. Keeping a continuously live proprietary connection is a different requirement and may rule out this option.

### 3. Back off prolonged bonded reconnect searches

**Priority: high for an absent or powered-down receiver.** Keep the validated 3.75 ms receive window, but consider increasing the interval between attempts after an initial fast-search period, eventually using a long sleep or host-controlled retry. Reset to fast search on key activity or explicit transport selection.

For illustration, retaining one 3.75 ms window while increasing its period from 20 ms to 100 ms changes the nominal RX-window duty from **18.75% to 3.75%**. This is a fivefold reduction in that duty component, **not** a measured fivefold battery-life improvement. A single infrequent beacon can miss a receiver scanning another channel, so a burst across the pairing channels may be more reliable than a literal interval substitution.

Production's roughly one-second/10 ms attempts and three-failure transition provide a concrete comparison, but copying its packet cadence is not justified without receiver testing. Test stock receivers and OpenDongle after cold boot, reset, USB suspend/resume, and long absence; measure both reconnect-tail latency and joules consumed while searching.

### 4. Keep protocol reset and disable semantics coherent

**Priority: medium, supporting reliable sleep adoption.** Document the `A6 56` incompatibility and production's real `A6 58` disable operation. A compatible disable path or richer capability response could simplify integration, but its meaning and persistence must be explicit. Production defaults must not be blindly persisted in OpenController while a legacy host can still send a wake-unaware stream.

There is also a source-level cancellation gap worth addressing in future work: `SleepProtocol_OnFrame(A6, 56)` clears the reducer's pending request, but an explicit request already transferred by `Power_Service` lives in `pwr_explicit_request`. `handle_uart_frame` only cancels that second latch for `SleepProtocol_IsStateChanging`, which excludes `A6 56`. Thus renegotiation while the callback is vetoing/delaying an already transferred request can leave a stale explicit sleep armed. The existing reducer test covers only the first latch. Add integration coverage across the service-to-callback handoff when fixing it.

### 5. Measure secondary costs after the policy changes

**Priority: lower.** Compare a release profile with `RF_DIAG_COUNTERS=0` against the default diagnostic build. Assess unnecessary UART chatter and scheduled work. A 100 ms post-wake holdoff costs much more than a few RTC ticks if every burst resets it; tune it only after a reliable handshake makes the host's needs clear.

The 200 ms shallow heartbeat is necessary with the current watchdog scheme. A future deadline-aware idle timer could combine watchdog servicing with the next TMOS deadline and reduce redundant wakes, but simply lengthening/removing the timer can starve both watchdog feeding and polled software timers. There is no reason from this binary to disable the watchdog for power savings.

Only then assess wake margin, RTC post-wake synchronization, flash fetches, and regulator/oscillator tuning with current traces over voltage and temperature. OpenController already restores HSE bias and enables supported DC-DC operation. Removing vendor power-plan steps or shrinking retained RAM without relocating all live state would be a much riskier experiment than reducing unnecessary radio-on time.

### Changes not justified by this comparison

- Removing the connected-link deep-sleep veto: the polled hop deadline remains invisible to TMOS.
- Replacing deep WFI with WFE: OpenController source records hardware lockups with that experiment.
- Reducing the reconnect receive window below six ticks without new interoperability evidence.
- Replacing retained sleep with shutdown/reset: wake would lose execution context and incur boot/reconnect costs.
- Copying production's weaker TX/RX checks or its zero return when no sleep was entered.

## Evidence limits and suggested measurements

Current comments record a DC-DC improvement of **7.17 mA to 5.03 mA** on the original OpenController board, older **0.68 mA idle / 7.75 mA searching** observations, and a retained-sleep floor below the bench meter's approximately **10 µA resolution**. These are historical, configuration-specific observations, not fresh measurements at the reviewed commit and not a production-versus-OpenController current comparison. They justify investigating radio duty and sleep residency but cannot establish the production image's battery-life advantage.

| Measurement | What it resolves |
|---|---|
| Same board, supply, peer, and scripted host traffic; production versus OpenController | Actual active/idle/sleep currents and wake energy without hardware or host confounding |
| Production at 5 s, several reconnect checks, and 30 min; repeat with flags disabled | Confirms the reconstructed two-stage policy and timer-setting nuance in a running unit. The 5 s entry and the one-per-second reconnect cadence are now Observed; the 30 min stage and the flag variants remain unmeasured |
| OpenController before negotiation, after `A6 57`, and after explicit `A6 54` | Distinguishes compiled capability from real deep-sleep residency |
| Receiver absent for minutes to hours | Energy cost of unlimited search and safe backoff parameters |
| Logic-analyzer RX/TX plus power trace, wake at randomized entry phases | Preamble loss, GPIO race, interrupt storms, first-key latency, and complete TX drain |
| Held keys, rapid key-up/down, Caps Lock changes, and reconnect with both peer implementations | Whether inactivity transitions preserve user-visible input and status behavior |
| RTC wrap, calibration wake, watchdog-enabled long sleep, and firmware update from sleep | Deadline arithmetic, wake restoration, and recovery behavior |

Measure the host MCU and other keyboard loads as well if the target is whole-keyboard battery life. A module-current improvement alone does not establish a battery-life percentage. Compare average current as time-weighted state residency plus wake/transition charge; keep RX duty, MCU awake duty, and measured rail current distinct.

## Source documentation discrepancies

These were left unchanged, in accordance with the consultation scope:

- [firmware/README.md](firmware/README.md) still describes deep sleep as follow-up work despite the committed implementation and enabled board knobs.
- [sleep_protocol.h](firmware/src/sleep_protocol.h) and related comments say stock has no disable opcode; the reviewed production binary implements `A6 58` for 2.4 GHz and `A6 56` for BLE later-stage auto-sleep disable.
- Some [power_sleep.c](firmware/src/power_sleep.c) comments say RF IDLE is refused by the idle callback or the runtime path is inert. The current callback handles both requested IDLE sleep and authorized bonded-search slices.
- Those comments also describe an approximately 17 ms search gap; the current six-tick RX window leaves at most approximately 16.25 ms before beacon/transition overhead, with the early-wake margin reducing actual deep-sleep residency further.
- [main.c](firmware/src/main.c) labels `0x63` "factory 2.4G pair" as if it were a stock command; the production image acknowledges it and does nothing (Observed). Its pairing path is `0x30` then `0x51`, which OpenController also implements.

## Bench observations (10 September 2026)

Method. `firmwareB-HalSleepFalse` on a WeAct CH592F devboard (the OpenController bench board), UART1 on its remapped PB12/PB13 through a WCH-LinkE serial bridge at 115,200 baud, and OpenDongle (CH592, its merged hop-model build) as the 2.4 GHz receiver, whose supervision, reacquire and promote counters were the instrument. The image was flashed over SWD and its DataFlash bond page erased so it booted unbonded; the receiver's bond was cleared over its maintenance interface. Every frame was preceded by a `0x00` wake byte and a 300 ms gap, and an all-keys-up `A1` report every 2 s held the module out of its first inactivity stage during measurements. No current was measured, and the receiver was OpenDongle, not the production dongle.

| Observation | Result |
|---|---|
| UART pins and rate | Remapped PB12/PB13 at 115,200 baud; nothing transmitted at boot |
| Generic acknowledgement | `61 0D 0A` from the module after every valid frame, status frames repeated three times |
| Transport select `A6 30` | `5B 34` then `5B 36` unbonded; `5B 32` then `5B 23` after a bonded reconnect completed within the second |
| Pairing | `A6 30` then `A6 51` → `5B 31`; the camping receiver connected within the same second. `A6 63` alone: acknowledgement only, no radio activity seen by the receiver |
| First inactivity stage | Entered 5.0 s after the last validated frame (4.98, 4.98, 5.01 s measured from a frame to the receiver's lapse; frames every 4 s never trigger it); the link then stopped and reconnected with a period of 1.010 s (standard deviation 15 ms over 15 cycles), each connection lasting on the order of 10 ms (46 ms at a 25 ms sampling floor); every short search was caught, so the failure counter never reached three |
| Wake byte | A bare frame after stage entry drew no reply; `0x00`, 300 ms, frame was answered every time |
| Sleep primitive versus ladder | The ladder ran unchanged with the sleep callback neutered, matching the 11-byte image difference |
| Link under injected gaps | With the keepalive running, the module tracked the receiver's connected hop through masked gaps of 1 to 13 poll slots and the 5 and 6 slot remainder bands, 30 of 30 |

After more than half an hour without frames the module had stopped probing and reconnected only on an explicit `A6 30`, consistent with the later stage; its timing was not pinned because the receiver lost power during that interval. Still unmeasured after this run: current in any state, the later stage's timing, the persistent-flag variants, wake gaps shorter than 300 ms, the real host MCU's traffic, and behaviour against the production receiver.

## Reproduction and evidence index

All production addresses are runtime addresses unless explicitly identified as file offsets. The image hash and startup mapping above are sufficient to relocate them in another disassembler.

| Address | Identification / evidence |
|---|---|
| `0x46C4` | Startup memory-copy loops, GP/SP, vector base, entry point |
| `0x99C0` | 60 MHz boot selection, GPIO parking, conditional DC-DC enable |
| `0x9668` | Remapped UART1 initialization on PB12/PB13 |
| `0x6E56`–`0x6E68` | Sleep callback pointer installed into library initialization structure |
| `0x7044`–`0x711F` | MCU sleep permission, TX/RX checks, RTC arithmetic, retained sleep and HSE restore |
| `0x7120` / `0x6EE6` | RTC wake initialization / trigger programming |
| `0x20000298`–`0x20000579` | Retained-sleep primitive; WFI at `0x200004B4` |
| `0x20000C2E` / `0x20001818` | RTC ISR / UART-RX GPIO wake ISR |
| `0x55A0` | GPIO-B interrupt configuration; argument zero selects low level |
| `0x7524` / `0x786C` | First-stage timer / conditional thirty-minute timer |
| `0x7C54`–`0x8151` | Inactivity, probe, full-sleep, and wake task events |
| `0x721A` / `0x725E` / `0x7334` | RF search stop / RF teardown / RF search start |
| `0x20000C7E` / `0x20000D2E` | RF state compare / state assignment |
| `0x20001192` | RF receive/state callback; successful low-power reconnect clears sleep permission |
| `0x20001678` / `0x8152` / `0x8346` | UART frame parser / frame dispatcher / A6 subcommand handler |
| `0x9A40` / `0x9B04` | Persistent configuration write / load and fallback defaults |
| `0x20001988` / `0x2000210E` / `0xAF38` | Application loop / TMOS processing / idle callback dispatch |
| `0x6D04` | HAL timer work, including the 192,000-tick / 120 s calibration event |

Primary local hardware references are the pinned WeActStudio [CH592 SFR definitions](third_party/weactstudio-wch-ble-core/Examples/CH592/ble/broadcaster/StdPeriphDriver/inc/CH592SFR.h), [GPIO definitions](third_party/weactstudio-wch-ble-core/Examples/CH592/ble/broadcaster/StdPeriphDriver/inc/CH59x_gpio.h), [power driver](third_party/weactstudio-wch-ble-core/Examples/CH592/ble/broadcaster/StdPeriphDriver/CH59x_pwr.c), and [HAL sleep wrapper](third_party/weactstudio-wch-ble-core/Examples/CH592/ble/broadcaster/ble/HAL/SLEEP.c). The pinned OpenWCH [BLE API header](third_party/openboot/third_party/openwch/ch592/EVT/EXAM/BLE/LIB/CH59xBLE_LIB.h) supplies the TMOS timer units and BLE role constants. These definitions support register/API identification; production policy conclusions come from the binary itself.

Verification comprised binary/HEX identity checks, instruction/decompiler cross-checks, all references to the production sleep-permission byte, and comparison with the current source and board defaults. The existing host sleep-reducer pytest suite was attempted, but the default Python environment lacks `pytest`; it was not executed. No firmware build, and no hardware verification of any OpenController source change, is claimed. The bench observations recorded above are a separate, claimed body of evidence: they were taken on hardware against a flashed production `HalSleepFalse` image and are measurements of the PRODUCTION behaviour this report reconstructs, not validation of OpenController changes. The only repository addition from this review is this report, including its bench addendum.
