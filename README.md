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
- `third_party/` — pinned git submodules used by the firmware.
- `tools/` — development and support tooling.

## Licensing

- Firmware and tools software: [Apache License 2.0](./firmware/LICENSE).
- Hardware designs: [CERN Open Hardware Licence Version 2 - Weakly Reciprocal](./hardware/LICENSE).
- The submodules under `third_party/` contain third-party code under their
  own terms
