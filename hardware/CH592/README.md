# OpenController CH592

OpenController CH592 is a 10.16 mm x 20.5 mm castellated 2.4 GHz wireless module for keyboards, built around the WCH CH592D RISC-V BLE MCU. It has the same outline, castellated pinout, PCB antenna, and 0.8 mm stackup as the [CH572](../CH572/) module, so a host PCB laid out for one takes the other. Like the CH572, it uses the independently created, WCH-informed PCB antenna from [OpenDongle](https://github.com/openkeyboard-org/OpenDongle).

This is the first CH592 revision and no boards have been built yet. The existing OpenController firmware targets the CH592F; it needs a CH592D board profile for this module (see [Known issues](#known-issues-and-next-revision)).

## Design files

| File or directory | Purpose |
| --- | --- |
| `OpenController-CH592.kicad_pro` | KiCad 10 project and design-rule settings |
| `OpenController-CH592.kicad_sch` | Schematic |
| `OpenController-CH592.kicad_pcb` | PCB layout |
| `lib/OpenController.kicad_sym` | Project-owned CH592D symbol |
| `lib/OpenController.pretty/` | Castellated-edge and CH592D QFN-20 footprints drawn for this project, and the antenna footprint copied unchanged from OpenDongle (see [`../NOTICE`](../NOTICE)) |
| `sym-lib-table`, `fp-lib-table` | Portable project-local library configuration |
| `fab/` | Exported outputs: schematic PDF and gerber/NC drill set (see [Fabrication outputs](#fabrication-outputs)) |

Open the project with KiCad 10 or later. The project-owned symbol and footprints resolve through `${KIPRJMOD}` paths in the local library tables. The remaining parts (passives, inductor, crystal, LDO, connector symbol) come from KiCad 10's standard libraries. The schematic and board carry embedded copies of every symbol and footprint, so they open without those libraries, but updating parts from the library needs them installed.

## Host interface

J1 is nine castellated half-holes (1.0 mm drill), three on each of three edges, all on one 0.1 in (2.54 mm) grid so a host can also use standard 0.1 in pin headers. Viewed from the top with the antenna at the bottom, the pads are numbered counter-clockwise from the top-left. The top silkscreen dot marks pad 1. Pad positions and functions match the CH572 module, except that the CH592D has no reset pin and its USB pins are separate from SWD.

| Pad | Edge | Signal | CH592D pin | Direction | Notes |
| ---: | --- | --- | --- | --- | --- |
| 1 | Left, top | GND | EP | - | |
| 2 | Left, middle | UART_TX | PB13 (8) | out | TXD1 on the UART1 remap, as used by the firmware and OpenBoot |
| 3 | Left, bottom | UART_RX | PB12 (9) | in | RXD1 on the UART1 remap |
| 4 | Right, bottom | NRST | - | - | Not connected: the CH592D has no reset pin |
| 5 | Right, middle | VCC | - | in | 3.3 V to 6.0 V supply into the LDO |
| 6 | Right, top | GND | EP | - | |
| 7 | Top, right | SWDIO | PB14 (7) | bidir | Two-wire debug data (TIO) |
| 8 | Top, middle | SWCLK | PB15 (6) | in | Two-wire debug clock (TCK), also the ROM ISP strap; do not hold low at power-up |
| 9 | Top, left | WAKE | PA11 (1) | out | Host wake line |

The board is 10.16 mm (0.4 in) wide, so the two side rows sit 0.4 in apart. Measured to the hole centres, the top pads are 2.54, 5.08, and 7.62 mm from the left edge, and the side pads are 2.54, 5.08, and 7.62 mm below the top edge. All I/O is 3.3 V logic. The host PCB must keep copper, ground pour, and metal away from the antenna end of the module.

## Electrical design notes

- VCC feeds U2, a Torex XC6206P332MR-G 3.3 V LDO (pin 1 VSS, pin 2 VOUT, pin 3 VIN). C1 (1 uF) is its input capacitor. Its 6.0 V maximum input covers USB VBUS; from a 3.3 V host rail it runs in dropout, which the CH592D tolerates (1.75 V to 3.6 V supply).
- U2 feeds VIO33/VDD33 (pin 5), the CH592D's only external supply input; VDCID, VDCIA, and VINTA are internal regulator nodes fed from it. C2 (2.2 uF) sits at pin 5 and is both the VDD33 capacitor the datasheet asks for with the DC-DC enabled (2.2 uF or 1 uF) and the LDO output capacitor. OpenDongle CH592 likewise uses a single capacitor there.
- The internal DC-DC converter is fitted. VSW (pin 4) drives L1 (10 uH, the datasheet's recommended value) into VDCID (pin 3). C4 (4.7 uF, the recommended value) decouples VDCID, and VDCIA (pin 16) is tied to VDCID as the datasheet recommends. C5 (470 nF) decouples VINTA (pin 14).
- The DC-DC is off at power-on. The CH592D then supplies VSW directly from VDD33, and L1 passes it through to VDCID. The firmware's `opencontroller-ch592` profile turns the converter on (`KBD_DCDC_ENABLE := 1`). WCH puts the operating current with the DC-DC running at about 60% of the direct-supply figure. OpenDongle CH592 makes the opposite choice: it leaves the DC-DC off and ties VSW straight to VDCID.
- JLC has no Basic inductor suited to the DC-DC: its Basic 10 uH parts (C1035, C1046) are signal inductors rated 3 mA and 15 mA. L1 is a TDK MLZ1608M100WT000, a 0603 multilayer inductor made for DC-DC power lines, with 1.05 ohm DCR and a 90 mA rating in the JLC listing.
- Y1 is a 32 MHz, 8 pF-load crystal on X32MI/X32MO (pins 13 and 12), the same part as the CH572 module. No external load capacitors are fitted; the CH592 provides programmable internal load capacitance (`R8_XT32M_TUNE`), and OpenDongle CH592 also runs an 8 pF crystal on it.
- The RF output (ANT, pin 15) drives the PCB antenna directly, as on OpenDongle.
- GND reaches the CH592D only through the exposed pad (pin 21 in the footprint), which has four 0.4/0.2 mm thermal vias to the bottom ground plane.
- U1's footprint was drawn for this project from the CH592 datasheet's QFN20 drawing (chapter 21: 3.0 +/- 0.1 mm body, 1.9 +/- 0.2 mm exposed pad, 0.25 +/- 0.07 mm leads). KiCad's generic `QFN-20-1EP_3x3mm_P0.4mm_EP1.65x1.65mm` would not do: its lands reach 1.025 mm from the package centre, inside the 1.05 mm edge of a maximum-size CH592D exposed pad. Here the lands run from 1.20 mm to 1.85 mm (0.65 mm x 0.2 mm), 0.15 mm clear of the largest exposed pad. On a nominal 3.0 mm body a longest lead overhangs their inner end by 0.02 mm, and by up to 0.07 mm on a smallest 2.9 mm body. The exposed-pad land is the nominal 1.9 mm. The four thermal vias are built into the footprint, 0.7 mm from the centre along each axis, near the exposed-pad corners. The exposed-pad solder paste is a central window plus four edge windows, 51% of the pad with their rounded corners, each at least 0.075 mm clear of the via copper, so no paste sits over a via barrel where solder could wick away.
- The CH592D's QFN-20 package does not bond out the RST pin, so pad 4 (NRST) is not connected. Reset the module by cycling VCC or over SWD.
- PB15 is the CH592D's ROM ISP strap: held low at power-on, it starts the ROM bootloader. The boot ROM pulls PB15 up, so an open pad 8 boots normally, but a host must not hold pad 8 low during power-up. USB (PB10/PB11) is not brought out, so this module is programmed over two-wire debug rather than ROM ISP. Two-wire debug is enabled from the factory (`CFG_DEBUG_EN`, datasheet Table 2-3), so a WCH-LinkE on pads 7 and 8 programs a blank part, and OpenBoot updates the firmware over the UART after that.
- Unused GPIOs (PA10, PA12 to PA15, PB10, PB11) carry no-connect flags.
- AE1 and J1 are PCB features, not assembly parts. Their footprints are excluded from the BOM and position file.

## Layout notes

- The outline, antenna, ground pours, and via fence are the CH572 module's: the antenna sits along the bottom edge of the 10.16 mm wide board, centred with about 0.58 mm side margins, and both ground pours stop at its inner edge (y = 117.205 mm) behind a 0.5/0.2 mm via fence at 0.75 mm pitch. The two vias beside the antenna's ground pad sit 0.1 mm to 0.15 mm closer to the board edge than on the CH572 to clear the ANT trace.
- The CH592D has ANT, VINTA, X32MI, and X32MO on one side of the package. U1 follows the RF corner of OpenDongle CH592, rotated 90 degrees with the antenna: it sits where U3 sits relative to the antenna feed and via fence there, moved 0.1 mm further from the fence, so that C5 fits in the strip between U1 and the fence without the courtyards overlapping.
- The ANT trace is 0.15 mm wide and about 4.8 mm long (about 4.3 mm on OpenDongle CH592). It runs along U1's pin row and then down beside C5 to the antenna feed.
- Y1 sits at U1's corner on the antenna side. X32MI runs between it and the via fence (0.15 mm, about 7.1 mm) and X32MO into its near pad (about 3.6 mm). The crystal's two ground pads each have a via.
- L1 and C4 sit between U1 and the castellated edge, and VDCIA reaches VDCID around U1's right-hand side. C2 sits beside VDD33, and the LDO and C1 sit between them and the castellated edge.
- Only two signals use the bottom layer: SWDIO from a via at U1's left-hand side, crossing under U1 between its two rows of thermal vias, and WAKE from a via inside the VDCIA loop. Everything else on the bottom layer is ground. SWDIO runs close to the right-hand castellations, so the bottom-layer ground beside pad 6 is a separate piece that joins through pad 6 itself and a stitching via; the rest of the bottom pour is one piece.
- Each castellated pad is 1.5 mm x 3.0 mm and straddles the board edge, leaving 1.5 mm of pad on the board and a 0.25 mm annular ring along the edge. The outermost holes are 2.54 mm (centre) from the corners, so every pad edge is at least 1.79 mm from the adjacent board edge, and hole edges are 1.54 mm apart. See [Known issues](#known-issues-and-next-revision) for JLCPCB's corner guidance.

## Design rules

The board-setup constraints are set to JLCPCB's 1-2 layer, 1 oz capabilities, as on the CH572. The design itself uses larger values (0.2 mm default track and clearance, 0.5/0.2 mm vias).

| Constraint | Setting |
| --- | --- |
| Minimum clearance / track width | 0.10 mm / 0.10 mm |
| Minimum via diameter / annular ring | 0.25 mm / 0.05 mm |
| Minimum through hole | 0.15 mm |
| Hole to hole / hole to copper | 0.20 mm / 0.20 mm |
| Copper to board edge | 0.20 mm (castellated pads are exempt) |
| Silkscreen text height / thickness | 1.0 mm / 0.15 mm |
| Solder mask minimum web | 0.10 mm, 1:1 expansion |

KiCad's board setup has a single hole-to-hole minimum and no pad-to-silkscreen minimum, so two JLCPCB limits are not checked by DRC: 0.45 mm between plated pad holes, and 0.15 mm from pads to silkscreen. The layout meets both: the closest plated pad holes are U1's thermal vias, 1.2 mm apart edge to edge (the castellations are 1.54 mm apart), and silkscreen stays at least 0.2 mm from every pad opening.

## Design verification

```sh
kicad-cli sch erc --severity-all OpenController-CH592.kicad_sch
kicad-cli pcb drc --severity-all --schematic-parity OpenController-CH592.kicad_pcb
```

ERC reports 0 errors and 0 warnings. DRC reports 0 violations, 0 unconnected items, and 0 schematic parity issues.

## Fabrication outputs

`fab/` holds the exported outputs: the schematic as PDF, and the gerber/NC drill set under `fab/gerbers/`. The KiCad sources remain the design of record; regenerate the exports with KiCad 10 whenever the schematic or layout changes:

```sh
kicad-cli sch export pdf -o fab/OpenController-CH592-schematic.pdf OpenController-CH592.kicad_sch
kicad-cli pcb export gerbers -o fab/gerbers/ \
    -l F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts \
    --no-protel-ext --check-zones OpenController-CH592.kicad_pcb
kicad-cli pcb export drill -o fab/gerbers/ --excellon-separate-th \
    --generate-map --map-format gerberx2 OpenController-CH592.kicad_pcb
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
| U1 | 1 | WCH CH592D | QFN-20, 3 mm x 3 mm, 0.4 mm pitch, exposed pad | C41417128 | Extended | Same part as OpenDongle CH592. Confirm pin-1 orientation and exposed-pad soldering |
| U2 | 1 | Torex XC6206P332MR-G, 3.3 V LDO | SOT-23-3 | C5446 | Basic | 200 mA, 6.0 V maximum input |
| L1 | 1 | 10 uH, TDK MLZ1608M100WT000 | 0603 | C76798 | Extended | DC-DC inductor; JLC's Basic 10 uH parts are 3 mA and 15 mA signal inductors |
| Y1 | 1 | 32 MHz crystal | 2016, 4 pad | C37635332 | Extended | 8 pF load, +/-10 ppm, 40 ohm maximum ESR; same part as the CH572 module. JLC lists no Basic 32 MHz crystal |
| C1 | 1 | 1 uF | 0603 | C15849 | Basic | X5R, 50 V |
| C2 | 1 | 2.2 uF | 0603 | C23630 | Basic | X5R, 16 V |
| C4 | 1 | 4.7 uF | 0603 | C19666 | Basic | X5R, 16 V |
| C5 | 1 | 470 nF | 0603 | C1623 | Basic | X7R, 25 V |

AE1 (PCB antenna) and J1 (castellated edge) are PCB features, not assembly parts.

## Known issues and next revision

- JLCPCB's [castellated PCB design guide](https://jlcpcb.com/blog/castellated-pcbs-introduction-and-design-requirements) recommends at least 3.0 mm (5.0 mm preferred) from a castellated hole to a board corner; the outermost holes here are 2.54 mm (centre) from the corners. That row appears only in the guide's expanded table, not on JLCPCB's capabilities page or in its Chinese design guide, and JLCPCB has built castellated boards with corner holes closer than this. Select castellated holes on three edges when ordering, and expect at most an engineering query.
- The antenna's ground plane is about 10.16 mm x 17 mm here against roughly 10 mm x 13 mm on OpenDongle, and the ANT trace is about 0.5 mm longer than OpenDongle CH592's. Check the match and range on the first boards, soldered to a representative host PCB.
- With VCC at 3.3 V the LDO has almost no headroom. Torex's usage notes ask for stability testing in that condition, so check it on the first boards if hosts will feed the module from a 3.3 V rail.
- The firmware has no CH592D board profile yet. The `opencontroller-ch592` profile already matches this module's UART1 remap (PB13/PB12) and fitted DC-DC inductor. Host wake needs firmware work: the firmware drives its wake line only on PB13, and its boot-time pin park pulls PA11 up until a driver claims it. A profile for this module must drive WAKE on PA11 and leave it out of the park.

## Source attribution

- The 0.8 mm FR-4 antenna footprint is copied from OpenDongle, where it was independently created, informed by the WCH reference antenna published in the [openwch schematic/PCB library](https://github.com/openwch/schpcb_lib); no WCH CAD or artwork file was copied, imported, converted, or traced.

## License

This hardware design, including the antenna implementation described in [`../NOTICE`](../NOTICE), is licensed under the CERN Open Hardware Licence Version 2 - Weakly Reciprocal. See [`LICENSE`](LICENSE).
