# Original OpenController CH592F bench/module hardware.
# UART1 is routed on its PB12/PB13 alternate mapping and RF pairing retains
# the historical fixed keyboard identity for compatibility with existing
# bench bonds.
OPENBOOT_BOARD := opencontroller-ch592
KBD_UART1_REMAP := 1
KBD_FACTORY_MAC := 0

# Run the core from the DC-DC converter.  Bench-measured 2026-09-02 on this
# board: 5.03 mA vs 7.17 mA active (-29.8%), full boot to the main loop
# verified via the phase sentinel with the converter running -- the inductor
# is populated.  Note the RF timing results predate the switch and were
# measured on the LDO; revalidate against a receiver when convenient.
KBD_DCDC_ENABLE := 1
