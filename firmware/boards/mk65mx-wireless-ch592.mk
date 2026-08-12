# MonacoKeys MK65MX Wireless prototype, CH592F sidecar.
# UART1 is on the silicon-default PA8/RXD1 and PA9/TXD1 pins.  PB13 is the
# board's CHWAKE output, so this profile must never enable the UART1 remap.
OPENBOOT_BOARD := mk65mx-wireless-ch592
KBD_UART1_REMAP := 0

# Each production keyboard uses the unique six-byte MAC stored in CH592 ROM
# as its 2.4 GHz pairing identity.  Invalid ROM data rejects pairing while
# leaving the host UART and OpenBoot entry path operational.
KBD_FACTORY_MAC := 1
