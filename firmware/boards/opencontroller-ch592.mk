# Original OpenController CH592F bench/module hardware.
# UART1 is routed on its PB12/PB13 alternate mapping and RF pairing retains
# the historical fixed keyboard identity for compatibility with existing
# bench bonds.
OPENBOOT_BOARD := opencontroller-ch592
KBD_UART1_REMAP := 1
KBD_FACTORY_MAC := 0

# Stay on the LDO.  The bench module's DC-DC inductor is not assumed
# populated, and this profile's supply behaviour is the one the RF timing
# results were measured against.
KBD_DCDC_ENABLE := 0
