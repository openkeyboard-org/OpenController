# OpenController Hardware

OpenController hardware is a small castellated 2.4 GHz wireless module that solders onto a keyboard PCB beside the host MCU and speaks the OpenController UART protocol. This directory contains the KiCad design files, project-local libraries, and bill-of-materials information for each implementation.

## Implementations

| Implementation | MCU | Board size | Power | Status |
| --- | --- | --- | --- | --- |
| [CH572](CH572/) | WCH CH572D (QFN-20) | 10.0 mm x 20.5 mm | VCC via XC6206 3.3 V LDO | First revision, not yet built |

The module reuses OpenDongle's independently created, WCH-informed 2.4 GHz PCB antenna and its two-layer 0.8 mm stackup, so the antenna sees the construction it was tuned for. The host connects through eight castellated half-holes carrying power, UART, a wake line, reset, and the SWD/USB pins used for programming.

See each implementation's README for detailed electrical, fabrication, and assembly information.

## Shared fabrication constraints

| Parameter | Requirement |
| --- | --- |
| Layer count | 2 copper layers |
| Finished thickness | 0.8 mm nominal (35 um + 0.73 mm core + 35 um) |
| Material | FR-4 |
| Copper | 1 oz / 35 um outer copper |
| Minimum finished drill | 0.20 mm |
| Minimum track / clearance used | 0.15 mm track, 0.2 mm clearance |
| Castellated holes | Yes |

## Verifying a design

Each project opens with KiCad 10 or later. Project-owned symbols and footprints resolve from the project's own directory via `${KIPRJMOD}`, and the rest come from KiCad 10's standard libraries, so nothing needs to be added to your global library tables.

```sh
kicad-cli sch erc --severity-all hardware/CH572/OpenController-CH572.kicad_sch
kicad-cli pcb drc --severity-all --schematic-parity hardware/CH572/OpenController-CH572.kicad_pcb
```

## License

The hardware designs are licensed under the CERN Open Hardware Licence Version
2 - Weakly Reciprocal. See [LICENSE](LICENSE). Design provenance, including the
antenna implementation shared with OpenDongle, is recorded in [NOTICE](NOTICE).
