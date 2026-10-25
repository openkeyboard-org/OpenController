# OpenController CH572

OpenController CH572 is a 10.16 mm x 20.5 mm castellated 2.4 GHz wireless module for keyboards, built around the WCH CH572D RISC-V wireless MCU. It solders onto the keyboard PCB beside the host MCU and talks to it over UART, using the same independently created, WCH-informed PCB antenna and 0.8 mm stackup as [OpenDongle](https://github.com/openkeyboard-org/OpenDongle).

This is the first CH572 revision and no boards have been built yet. The existing OpenController firmware targets the CH592F; a CH572 port is follow-up work.

## Design files

| File or directory | Purpose |
| --- | --- |
| `OpenController-CH572.kicad_pro` | KiCad 10 project and design-rule settings |
| `OpenController-CH572.kicad_sch` | Schematic |
| `OpenController-CH572.kicad_pcb` | PCB layout |
| `lib/OpenController.kicad_sym` | Project-owned CH572D symbol |
| `lib/OpenController.pretty/` | Castellated-edge footprint, and the antenna footprint copied unchanged from OpenDongle (see [`../NOTICE`](../NOTICE)) |
| `sym-lib-table`, `fp-lib-table` | Portable project-local library configuration |
| `fab/` | Exported outputs: schematic PDF and gerber/NC drill set (see [Fabrication outputs](#fabrication-outputs)) |

Open the project with KiCad 10 or later. The project-owned symbol and footprints resolve through `${KIPRJMOD}` paths in the local library tables. The remaining parts (passives, crystal, LDO, QFN package, connector symbol) come from KiCad 10's standard libraries. The schematic and board carry embedded copies of every symbol and footprint, so they open without those libraries, but updating parts from the library needs them installed.

## Host interface

J1 is nine castellated half-holes (1.0 mm drill), three on each of three edges, all on one 0.1 in (2.54 mm) grid so a host can also use standard 0.1 in pin headers. Viewed from the top with the antenna at the bottom, the pads are numbered counter-clockwise from the top-left. The top silkscreen dot marks pad 1.

| Pad | Edge | Signal | CH572D pin | Direction | Notes |
| ---: | --- | --- | --- | --- | --- |
| 1 | Left, top | GND | EP | - | |
| 2 | Left, middle | UART_TX | PA3 (1) | out | Default UART TXD, as used by OpenBoot's CH57x port |
| 3 | Left, bottom | UART_RX | PA2 (20) | in | Default UART RXD |
| 4 | Right, bottom | NRST | PA8 (10) | in | Default RST pin; reset input is disabled until `CFG_RESET_EN` is set |
| 5 | Right, middle | VCC | - | in | 3.3 V to 6.0 V supply into the LDO |
| 6 | Right, top | GND | EP | - | |
| 7 | Top, right | SWDIO | PA0 (11) | bidir | Also USB D- and UART ISP |
| 8 | Top, middle | SWCLK | PA1 (12) | bidir | Also USB D+ and the ROM boot strap; 100 kOhm pull-down (R1) |
| 9 | Top, left | WAKE | PA4 (13) | out | Host wake line |

The board is 10.16 mm (0.4 in) wide, so the two side rows sit 0.4 in apart. Measured to the hole centres, the top pads are 2.54, 5.08, and 7.62 mm from the left edge, and the side pads are 2.54, 5.08, and 7.62 mm below the top edge. All I/O is 3.3 V and none of the J1 signals is 5 V tolerant. The host PCB must keep copper, ground pour, and metal away from the antenna end of the module.

## Electrical design notes

- VCC feeds U2, a Torex XC6206P332MR-G 3.3 V LDO (pin 1 VSS, pin 2 VOUT, pin 3 VIN). C1 (1 uF) is its input capacitor. Its 6.0 V maximum input covers USB VBUS; from a 3.3 V host rail it runs in dropout, which the CH572 tolerates (2.0 V to 3.6 V low-voltage supply mode).
- The CH572D runs in single 3.3 V supply mode: V5 (pin 8) and VIO33 (pin 7) are tied to VDD33 (pin 6), as datasheet Table 1-1 allows. The three pins are joined at the pads. C3 (100 nF) sits at pin 6 and C2 (2.2 uF) is both the VDD33 bulk capacitor and the LDO output capacitor, matching the datasheet's 2.2 uF plus 0.1 uF requirement.
- Y1 is a 32 MHz, 8 pF-load crystal on XI/XO (pins 3 and 2). No external load capacitors are fitted; the CH572 provides programmable internal load capacitance.
- The RF output (ANT, pin 5) drives the PCB antenna directly, as on OpenDongle.
- GND reaches the CH572D only through the exposed pad (pin 21 in the footprint, pin 0 in the datasheet), which has four 0.2 mm vias to the bottom ground plane.
- NRST uses PA8, the factory-default reset pin (datasheet Table 2-3: `CFG_RST_PIN_EN` = 0). The reset input itself is off by default; set `CFG_RESET_EN` in the configuration bits to use it.
- PA1 is the ROM boot strap: PA1 high with PA0 low at power-on enters USB/UART ISP. R1 holds PA1 low when pad 8 is left open, so a cold power-up never parks in the bootloader by accident. A WCH-LinkE on pads 7/8 or the D+ pull-up "boot cable" still overrides it.
- R1 is a deliberate trade-off against USB. USB 2.0 full speed asks for at least 300 kOhm input impedance on D+ (Table 7-7), and R1 is 100 kOhm. At 300 kOhm, the CH572's worst-case floating-input leakage (3 uA) would put PA1 exactly at VIL max (0.9 V) and the strap would no longer be guaranteed; at 100 kOhm it sits at 0.3 V or below. USB on this module is only an ISP path, and R1 moves the D+ idle level from about 3.0 V to 2.96 V against a 2.0 V threshold.
- Unused GPIOs (PA5, PA6, PA7, PA9, PA10, PA11) and the two NC pins carry no-connect flags.
- AE1 and J1 are PCB features, not assembly parts. Their footprints are excluded from the BOM and position file.

## Layout notes

- The antenna sits along the bottom edge, centred on a 10.16 mm wide board with about 0.58 mm side margins. That is 0.16 mm wider than OpenDongle, so the side castellations sit 0.4 in apart; the ground edge the antenna sees is 1.6% wider.
- The ground pours on both layers stop at the antenna's inner edge (y = 117.205 mm), and a 0.5/0.2 mm via fence runs about 0.25 mm inside that edge (via centres at y = 116.95 mm) at 0.75 mm pitch, following OpenDongle.
- The ANT trace is 0.15 mm wide and 2.7 mm long, from U1 pin 5 to the antenna feed.
- U1 is rotated so that ANT, XI and XO face the antenna. The crystal sits beside U1, and UART_TX/UART_RX drop to the bottom layer through two vias at U1's corner.
- The bottom layer carries the six castellated signal runs; everything else is ground.
- Each castellated pad is 1.5 mm x 3.0 mm and straddles the board edge, leaving 1.5 mm of pad on the board and a 0.25 mm annular ring along the edge. The outermost holes are 2.54 mm (centre) from the corners, so every pad edge is at least 1.79 mm from the adjacent board edge, and hole edges are 1.54 mm apart. See [Known issues](#known-issues-and-next-revision) for JLCPCB's corner guidance.

## Design rules

The board-setup constraints are set to JLCPCB's 1-2 layer, 1 oz capabilities. The design itself uses larger values (0.2 mm default track and clearance, 0.5/0.2 mm vias).

| Constraint | Setting |
| --- | --- |
| Minimum clearance / track width | 0.10 mm / 0.10 mm |
| Minimum via diameter / annular ring | 0.25 mm / 0.05 mm |
| Minimum through hole | 0.15 mm |
| Hole to hole / hole to copper | 0.20 mm / 0.20 mm |
| Copper to board edge | 0.20 mm (castellated pads are exempt) |
| Silkscreen text height / thickness | 1.0 mm / 0.15 mm |
| Solder mask minimum web | 0.10 mm, 1:1 expansion |

KiCad's board setup has a single hole-to-hole minimum and no pad-to-silkscreen minimum, so two JLCPCB limits are not checked by DRC: 0.45 mm between plated pad holes, and 0.15 mm from pads to silkscreen. The layout meets both: the closest pad holes (the castellations) are 1.54 mm apart, and silkscreen stays at least 0.2 mm from every pad opening.

## Design verification

```sh
kicad-cli sch erc --severity-all OpenController-CH572.kicad_sch
kicad-cli pcb drc --severity-all --schematic-parity OpenController-CH572.kicad_pcb
```

ERC reports 0 errors and 0 warnings. DRC reports 0 violations, 0 unconnected items, and 0 schematic parity issues.

## Fabrication outputs

`fab/` holds the exported outputs: the schematic as PDF, and the gerber/NC drill set under `fab/gerbers/`. The KiCad sources remain the design of record; regenerate the exports with KiCad 10 whenever the schematic or layout changes:

```sh
kicad-cli sch export pdf -o fab/OpenController-CH572-schematic.pdf OpenController-CH572.kicad_sch
kicad-cli pcb export gerbers -o fab/gerbers/ \
    -l F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts \
    --no-protel-ext --check-zones OpenController-CH572.kicad_pcb
kicad-cli pcb export drill -o fab/gerbers/ --excellon-separate-th \
    --generate-map --map-format gerberx2 OpenController-CH572.kicad_pcb
```

The gerber job file reports 10.21 mm x 20.55 mm and 0.84 mm. The size includes the outline's 0.05 mm line width and the thickness includes both 0.02 mm solder mask layers. Order the board as 10.16 mm x 20.5 mm and 0.8 mm.

Anyone ordering from these files must still pass the stackup, castellation, and antenna requirements below to the fabricator; the gerbers alone do not carry them. JLC-oriented assembly outputs from the Fabrication Toolkit plugin go to `production/`, which stays untracked.

## Fabrication requirements

The antenna is only tuned for this construction. Use these settings unless a revised PCB has been validated:

| Parameter | Requirement |
| --- | --- |
| Layer count | 2 copper layers |
| Finished board size | 10.16 mm x 20.5 mm |
| Finished thickness | 0.8 mm nominal (35 um + 0.73 mm core + 35 um) |
| Material | FR-4 |
| Copper | 1 oz / 35 um outer copper |
| Castellated holes | Yes, on three edges (9 half-holes, 1.0 mm) |
| Surface finish | ENIG recommended for the castellated pads |
| Minimum finished drill | 0.20 mm |
| Minimum track/space used | 0.15 mm track, 0.2 mm clearance |
| Soldermask | Both sides; no soldermask over the castellated pads or antenna conductor |

Preserve the antenna copper, ground edge, board thickness, and via fence exactly, and do not let the fabricator add serial numbers or tooling features in the antenna region. The enclosure and host PCB must not place metal or a ground pour over or directly under the antenna.

## Suggested BOM

The listed LCSC/JLCPCB numbers are suggested assembly parts, not sole-source requirements. Library type is as JLCPCB listed it on 2026-10-25.

| References | Qty | Value / part | Package | Suggested LCSC part | JLC library | Notes |
| --- | ---: | --- | --- | --- | --- | --- |
| U1 | 1 | WCH CH572D | QFN-20, 3 mm x 3 mm, 0.4 mm pitch, exposed pad | C46597863 | Extended | Confirm pin-1 orientation and exposed-pad soldering |
| U2 | 1 | Torex XC6206P332MR-G, 3.3 V LDO | SOT-23-3 | C5446 | Basic | 200 mA, 6.0 V maximum input |
| Y1 | 1 | 32 MHz crystal | 2016, 4 pad | C37635332 | Extended | 8 pF load, +/-10 ppm, 40 ohm maximum ESR; same part as OpenDongle CH570. JLC lists no Basic 32 MHz crystal |
| C1 | 1 | 1 uF | 0603 | C15849 | Basic | X5R, 50 V |
| C2 | 1 | 2.2 uF | 0603 | C23630 | Basic | X5R, 16 V |
| C3 | 1 | 100 nF | 0603 | C14663 | Basic | X7R, 50 V |
| R1 | 1 | 100 kOhm, 1% | 0603 | C25803 | Basic | PA1 boot-strap pull-down |

AE1 (PCB antenna) and J1 (castellated edge) are PCB features, not assembly parts.

## Known issues and next revision

- JLCPCB's [castellated PCB design guide](https://jlcpcb.com/blog/castellated-pcbs-introduction-and-design-requirements) recommends at least 3.0 mm (5.0 mm preferred) from a castellated hole to a board corner; the outermost holes here are 2.54 mm (centre) from the corners. That row appears only in the guide's expanded table, not on JLCPCB's capabilities page or in its Chinese design guide, and JLCPCB has built castellated boards with corner holes closer than this. Select castellated holes on three edges when ordering, and expect at most an engineering query.
- The antenna's ground plane is about 10.16 mm x 17 mm here against roughly 10 mm x 13 mm on OpenDongle. Check the match and range on the first boards, soldered to a representative host PCB.
- With VCC at 3.3 V the LDO has almost no headroom. Torex's usage notes ask for stability testing in that condition, so check it on the first boards if hosts will feed the module from a 3.3 V rail.
- The firmware has no CH572 board profile yet. The OpenBoot CH57x port already uses the PA3/PA2 UART mapping that J1 carries.

## Source attribution

- The 0.8 mm FR-4 antenna footprint is copied from OpenDongle, where it was independently created, informed by the WCH reference antenna published in the [openwch schematic/PCB library](https://github.com/openwch/schpcb_lib); no WCH CAD or artwork file was copied, imported, converted, or traced.

## License

This hardware design, including the antenna implementation described in [`../NOTICE`](../NOTICE), is licensed under the CERN Open Hardware Licence Version 2 - Weakly Reciprocal. See [`LICENSE`](LICENSE).
