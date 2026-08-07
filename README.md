# OpenController

OpenController is an open-source wireless keyboard controller designed to be compatible with OpenDongle.

## Firmware

The first firmware target is the WCH CH592F wireless module: a keyboard-side
2.4 GHz link (pairing, connected-mode hopping, HID uplink, persistent
bonding) driven by the keyboard's host MCU over UART, bench-validated
against OpenDongle. See [firmware/README.md](./firmware/README.md) for
toolchain, build, and flashing instructions.

## Repository layout

- `firmware/` — firmware source and related software artifacts.
- `hardware/` — hardware design files and documentation.
- `tools/` — development and support tooling.

## Licensing

- Firmware and tools software: [Apache License 2.0](./firmware/LICENSE).
- Hardware designs: [CERN Open Hardware Licence Version 2 - Weakly Reciprocal](./hardware/LICENSE).
- `firmware/vendor/` and the WCH-derived startup file
  `firmware/src/startup_CH592_phased.S` contain third-party WCH/WeActStudio
  code under its own terms — see [firmware/NOTICE](./firmware/NOTICE).