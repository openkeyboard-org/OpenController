# MonacoKeys MK65MX Wireless prototype, CH592F sidecar.
# UART1 is on the silicon-default PA8/RXD1 and PA9/TXD1 pins.  PB13 is the
# board's CHWAKE output, so this profile must never enable the UART1 remap.
OPENBOOT_BOARD := mk65mx-wireless-ch592
KBD_UART1_REMAP := 0

# Each production keyboard uses the unique six-byte MAC stored in CH592 ROM
# as its 2.4 GHz pairing identity.  Invalid ROM data rejects pairing while
# leaving the host UART and OpenBoot entry path operational.
KBD_FACTORY_MAC := 1

# The module populates the CH592 DC-DC inductor, so run the core from the
# switching regulator instead of the LDO.  Never set this for a board without
# that inductor: the converter would have no storage element and the core
# supply collapses.  PWR_DCDCCfg itself only guards the silicon capability
# (ROM_CFG_ADR_HW bit 13), not what the PCB populates.
KBD_DCDC_ENABLE := 1

# Deep-sleep plumbing (power ladder MR5): see opencontroller-ch592.mk.
KBD_DEEP_SLEEP := 1

# Host wake: the module raises CHWAKE (PB13 -> STM32 PA1) before every frame
# it sends, so the STM32 can sleep in STOP 2 without losing them, and answers
# the host's A6 58 query with 5B 38. Driven from boot. Needs PB13 free, so
# never with KBD_UART1_REMAP=1.
KBD_HOST_WAKE := 1
