# Vendored third-party code

Nothing in this directory is covered by the repository's Apache-2.0 license;
see `../NOTICE`. Do not edit files here locally — replace them wholesale from
upstream and record the new provenance below.

## wch-ch592-sdk/

Subset of the WCH CH59x SDK needed to build the firmware: the BLE HAL
(`ble/HAL/`), the peripheral drivers the app compiles
(`StdPeriphDriver/CH59x_{clk,gpio,sys,uart1,flash,pwr,adc}.c`), all
StdPeriphDriver headers (`CH59x_common.h` includes them unconditionally),
`libISP592.a` (DataFlash `EEPROM_READ/WRITE/ERASE`), and the RISC-V core
support (`RVMSIS/`).

- Upstream: <https://github.com/WeActStudio/WeActStudio.WCH-BLE-Core>
  at commit `1ee632c27b2caef52a65079a98a51e9bf7f42f61`, path
  `Examples/CH592/ble/broadcaster/`.
- Why the WeActStudio copies: the bench-validated firmware binary was built
  against exactly these files. They differ from the openwch upstream
  (<https://github.com/openwch/ch592>) in `CONFIG.h`, `CH592SFR.h`, and most
  drivers — switching to openwch requires a rebuild and bench re-validation.

## wch-ble-lib-v1.4.2/

WCH CH59x BLE library release V1.4.2 (closed-source `LIBCH59xBLE.a`, its API
header `CH59xBLE_LIB.h`, and the `ble_task_scheduler.S` LLE-IRQ trampoline).
Byte-identical to the triple shipped in the openwch CH592 EVT tree
(`EVT/EXAM/BLE/LIB/`). Other library versions are drop-in replacements for
this directory, selected at build time via `make BLE_LIB_DIR=...`.

The library is compiled with WCH's `WCH-Interrupt-fast` (HPE) semantics and
requires a MounRiver/WCH toolchain (GCC15 default, GCC12 supported) — see
`../README.md`.

## Checksums (sha256)

Scope: this manifest pins the two binary blobs and the complete BLE library
triple — the files whose provenance cannot be re-derived by reading them.
The remaining SDK text files are verified wholesale against the upstream
commit recorded above (every vendored file was copied verbatim and
sha256-compared against its source at import time).

```text
bf59ec76645f85240d5d3004680072643c71b35abed0901b5e4347c9241276bf  wch-ble-lib-v1.4.2/LIBCH59xBLE.a
cfd62a1d3473f5bde2a64a85aab3154320740b7d5f32df3d9a59c8db5a69d70c  wch-ble-lib-v1.4.2/CH59xBLE_LIB.h
5edaebcdabb1ffb49f4184a2c9890ee7a3dc7ff2d50d55d59912227bc5ae7a94  wch-ble-lib-v1.4.2/ble_task_scheduler.S
3aca22e7ea24d6011de90e5a865922834d2c5bb664734fa66536cf52acea6285  wch-ch592-sdk/StdPeriphDriver/libISP592.a
```

## Provenance notes

- `wch-ch592-sdk/StdPeriphDriver/inc/CH59x_i2c.h` ships upstream without
  the WCH copyright header that the other SDK headers carry. The file here
  is byte-identical to upstream; the header was not added locally, per the
  no-local-edits rule.
