# OpenController Hardware

OpenController hardware is a small castellated 2.4 GHz wireless module that solders onto a keyboard PCB beside the host MCU and speaks the OpenController UART protocol. This directory contains the KiCad design files, project-local libraries, and bill-of-materials information for each implementation.

## Implementations

| Implementation | MCU | Board size | Power | Status |
| --- | --- | --- | --- | --- |
| [CH572](CH572/) | WCH CH572D (QFN-20) | 10.16 mm x 20.5 mm | VCC via XC6206 3.3 V LDO | First revision, not yet built |
| [CH592](CH592/) | WCH CH592D (QFN-20) | 10.16 mm x 20.5 mm | VCC via XC6206 3.3 V LDO, internal DC-DC | First revision, not yet built |

Both modules reuse OpenDongle's independently created, WCH-informed 2.4 GHz PCB antenna and its two-layer 0.8 mm stackup, so the antenna sees the construction it was tuned for. They share one outline and pinout: the host connects through nine castellated half-holes, three on each of three edges on one 0.1 in grid, carrying power, two grounds, UART, a wake line, reset, and the SWD pins used for programming (also USB on the CH572). The CH592D has no reset pin, so the CH592 module leaves that pad unconnected.

```
   9 WAKE  8 SWCLK  7 SWDIO
  +---o-------o-------o---+
1 o GND             GND o 6
2 o UART_TX         VCC o 5
3 o UART_RX        NRST o 4
  |        ground         |
  |~~~~~~~ antenna ~~~~~~~|
  +-----------------------+
     top view, 2.54 mm pitch
```

Pad 1 is the uppermost pad on the left edge, and the numbering runs counter-clockwise: down the left edge (1-3), up the right edge (4-6), then right to left along the top (7-9). The top silkscreen dot marks pad 1.

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
kicad-cli sch erc --severity-all hardware/CH592/OpenController-CH592.kicad_sch
kicad-cli pcb drc --severity-all --schematic-parity hardware/CH592/OpenController-CH592.kicad_pcb
```

## License

The hardware designs are licensed under the CERN Open Hardware Licence Version
2 - Weakly Reciprocal. See [LICENSE](LICENSE). Design provenance, including the
antenna implementation shared with OpenDongle, is recorded in [NOTICE](NOTICE).
