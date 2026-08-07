# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 OpenController contributors
#
# OpenController keyboard wireless-module firmware — CH592F
# Target: WCH CH592F (QingKe RISC-V4F, 60MHz, 448KB flash, 26KB SRAM)
#
# The application runs under OpenBoot (third_party/openboot): bootloader at
# [0, 0x2000), two A/B application slots, per-slot boot records. Each build
# targets ONE slot (SLOT=A|B); releases ship both builds as one .obb bundle.
# See BOOT.md for the flash map and update flows.
#
# REQUIRED toolchain:
#
#   MounRiver/WCH GCC12:
#     riscv-wch-elf-gcc 12.2.0 / riscv-wch-elf-gdb 12.1
#     (override the install location with MRS_TOOLCHAIN=<path>)
#
# The Makefile defaults to that toolchain and INT_SOFT=0. Override only for
# explicit compiler A/B work; mainline-GCC builds are not bench-validated
# protocol evidence.
#
# The CH59x BLE library (LIBCH59xBLE.a) is compiled with WCH's custom
# `WCH-Interrupt-fast` attribute and requires it. Mainline GCC (xPack)
# with INT_SOFT=1 produces a binary that wedges silently — no IRQs
# delivered, chip alive but inert. The pinned WCH GCC12 toolchain (which
# supports the WCH attribute natively) produces a working binary. Bench-proven
# four-way matrix:
#
#   xPack + INT_SOFT=1: 0 bytes changed in 3s (DEAD)
#   xPack + INT_SOFT=0: 4 bytes changed (xPack silently falls back)
#   MRS   + INT_SOFT=1: 0 bytes changed (DEAD)
#   MRS   + INT_SOFT=0: 20 bytes changed (FULLY ALIVE)
#   WCH prebuilt RF_PHY.hex (reference): 13 bytes changed

# Grouped targets (&:) need GNU Make >= 4.3; older make silently races the
# factory rule instead of failing.
ifneq ($(shell printf '4.3\n%s\n' '$(MAKE_VERSION)' | sort -V -C && echo ok),ok)
$(error GNU Make 4.3 or newer is required; this is $(MAKE_VERSION))
endif
.DELETE_ON_ERROR:

# Overridable interpreter. Hermetic invocations (geometry, check-deps) add -I
# themselves; the pyserial update helper must NOT be isolated, or a
# `pip install --user pyserial` stops being importable.
PYTHON ?= python3

# SDK path (relative to this Makefile): the vendored WeActStudio SDK subset.
# Layout mirrors the WeAct example tree; see vendor/README.md for provenance.
SDK ?= vendor/wch-ch592-sdk

SDK_BLE_DIR := $(SDK)/ble
SDK_SRC_DIR := $(SDK)

# CH59x BLE library directory. Defaults to the vendored v1.4.2 triple
# (LIBCH59xBLE.a + CH59xBLE_LIB.h + ble_task_scheduler.S), the
# CH59x_BLE_LIB_V1.4.2 release. Other library versions (e.g. from the openwch
# CH592 EVT tree) are drop-in triples; override to A/B-compare:
#
#   make BLE_LIB_DIR=path/to/other-ble-lib
#
# NOTE: earlier bench notes thought v1.4.2 required app-side
# PFIC_EnableIRQ(BLEB/BLEL), but that was a debugger-reset artifact. The app
# now relies on the library's normal BLEB fast-vector setup; see
# docs/TMOS_REVIEW.md.
BLE_LIB_DIR ?= vendor/wch-ble-lib-v1.4.2

# Toolchain
MRS_TOOLCHAIN ?= $(HOME)/Development/Mounriver/Toolchain/RISC-V Embedded GCC12/bin
export PATH := $(MRS_TOOLCHAIN):$(PATH)

PREFIX ?= riscv-wch-elf-
CC     := $(PREFIX)gcc
AS     := $(PREFIX)gcc
LD     := $(PREFIX)gcc
OBJCOPY:= $(PREFIX)objcopy
SIZE   := $(PREFIX)size

# ---------------------------------------------------------------------------
# OpenBoot integration
# ---------------------------------------------------------------------------
OPENBOOT := $(abspath ../third_party/openboot)
OPENBOOT_BOARD := opencontroller-ch592
# The OpenBoot revision this build is pinned to. Declared rather than read from
# the submodule so it is a deterministic parse-time value, and asserted against
# the actual checkout by check-deps and again by the factory rule (manifest).
OPENBOOT_REVISION := c8ddc2f80dd126ebe20b3d0065951c16476a9f58
# Same GCC12 toolchain for app and bootloader (no two-toolchain split), but
# keep the hard gate below - upstream's own compiler check is advisory.
OPENBOOT_TOOLCHAIN ?= $(MRS_TOOLCHAIN)
OPENBOOT_CLI := $(OPENBOOT)/tools/target/release/openboot

# OpenBoot's A/B slot geometry, read out of the pinned submodule rather than
# copied here. openboot_app.c derives its boot-record address from
# SLOT_BASE + SLOT_SIZE, so a local copy that drifted from the bootloader's
# own build would silently point one slot away. openboot_geometry.py also
# cross-checks slot contiguity, alignment, and app-region containment.
#
# Skipped when the only goal is `clean`: a fresh checkout must be able to
# `make clean` before its submodules are initialized.
NEED_OPENBOOT_GEOM := 1
ifneq ($(MAKECMDGOALS),)
ifeq ($(filter-out clean,$(MAKECMDGOALS)),)
NEED_OPENBOOT_GEOM := 0
endif
endif
ifeq ($(NEED_OPENBOOT_GEOM),1)
OPENBOOT_GEOM := $(shell $(PYTHON) -I openboot_geometry.py \
    --openboot "$(OPENBOOT)" --chip ch592 --transport uart \
    --board $(OPENBOOT_BOARD))
ifneq ($(words $(OPENBOOT_GEOM)),6)
$(error cannot read OpenBoot slot geometry - run: git submodule update --init --recursive)
endif
OPENBOOT_SLOT_A_BASE := $(word 1,$(OPENBOOT_GEOM))
OPENBOOT_SLOT_SIZE   := $(word 2,$(OPENBOOT_GEOM))
OPENBOOT_SLOT_CAP    := $(word 3,$(OPENBOOT_GEOM))
OPENBOOT_RECORD_SZ   := $(word 4,$(OPENBOOT_GEOM))
OPENBOOT_SRC_BIN     := $(word 5,$(OPENBOOT_GEOM))
OPENBOOT_SLOT_B_BASE := $(word 6,$(OPENBOOT_GEOM))

# The bootloader's RAM boot-request mailbox address, parsed from the pinned
# protocol header rather than hardcoded: the app must reserve everything at
# and above it (stack top == mailbox address). Emitted into memory.ld as
# OC_BOOTREQ_ADDR so the linker asserts against the same source of truth.
OPENBOOT_BOOTREQ_ADDR := $(shell grep OB_BOOTREQ_ADDR_CH59X \
    "$(OPENBOOT)/protocol/openboot_protocol.h" | grep -o '0x[0-9A-Fa-f]*' | head -1)
ifeq ($(OPENBOOT_BOOTREQ_ADDR),)
$(error cannot read OB_BOOTREQ_ADDR_CH59X from the OpenBoot protocol header)
endif
endif

# Which slot this build targets. OpenBoot alternates its write target after
# every COMMIT, so a release needs BOTH builds: an update is refused unless
# the image's base equals the device's current write_base. Objects are
# compiled per slot rather than linked twice from one set, because the slot
# base reaches the compiler (openboot_app.c and kbd_image_id.c use it).
SLOT ?= A
ifeq ($(SLOT),A)
OPENBOOT_SLOT_BASE := $(OPENBOOT_SLOT_A_BASE)
else ifeq ($(SLOT),B)
OPENBOOT_SLOT_BASE := $(OPENBOOT_SLOT_B_BASE)
else
$(error SLOT must be A or B, got '$(SLOT)')
endif

# Project
TARGET := opencontroller_ch592f
BUILD  := build/ch592f-slot$(SLOT)
MEMORY_LD := $(BUILD)/memory.ld

SLOT_A_BIN := build/ch592f-slotA/$(TARGET).bin
SLOT_B_BIN := build/ch592f-slotB/$(TARGET).bin
BUNDLE_BIN := build/opencontroller-ch592f.obb

OPENBOOT_FACTORY_BIN := $(patsubst %.bin,%-factory.bin,$(OPENBOOT_SRC_BIN))
OPENBOOT_MANIFEST := $(patsubst %.bin,%.manifest,$(OPENBOOT_SRC_BIN))
FACTORY_BIN := $(BUILD)/$(TARGET)-factory.bin
FACTORY_MANIFEST := $(BUILD)/$(TARGET)-factory.manifest

# Architecture flags
ARCH   := -march=rv32imac_zicsr_zifencei -mabi=ilp32
ARCH   += -msmall-data-limit=8 -mno-save-restore

# Compiler flags
CFLAGS := $(ARCH) -O2 -g
CFLAGS += -std=gnu99
CFLAGS += -Wall -Wextra -Wno-unused-parameter
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -fno-common
CFLAGS += -DOPENBOOT_CHIP_CH59X
CFLAGS += -DOPENBOOT_SLOT_BASE=$(OPENBOOT_SLOT_BASE)
CFLAGS += -DOPENBOOT_SLOT_SIZE=$(OPENBOOT_SLOT_SIZE)

# Per-build overrides go through EXTRA_CFLAGS. Use `make EXTRA_CFLAGS=...`
# not `make CFLAGS+=...`; the latter clobbers ARCH.
CFLAGS += $(EXTRA_CFLAGS)

# SWD-readable .diag_safe counters: on by default (bench image). Build a release
# image with `make RF_DIAG_COUNTERS=0` to compile out the counters, their
# always-on read-modify-writes (incl. the RX-ISR hot path), and their SRAM.
# Passed to both .c and .S (AS is gcc).
RF_DIAG_COUNTERS ?= 1
CFLAGS += -DRF_DIAG_COUNTERS=$(RF_DIAG_COUNTERS)

# Independent WWDG watchdog (last-resort recovery for an unknown firmware hang),
# on by default. `make WATCHDOG_ENABLE=0` builds without it.
WATCHDOG_ENABLE ?= 1
CFLAGS += -DWATCHDOG_ENABLE=$(WATCHDOG_ENABLE)
# Bipolar-servo R8 fix (connected-mode link-lock), on by default.
# `make LINK_LOCK_SERVO_FIX=0` builds the original +1-only servo for the A/B crossover.
LINK_LOCK_SERVO_FIX ?= 1
CFLAGS += -DLINK_LOCK_SERVO_FIX=$(LINK_LOCK_SERVO_FIX)
# Post-poll response turnaround (TMR0 counts). 6000 ~= 100 us @ 60 MHz, validated
# against the production stock dongle; override for bench sweeps.
RF_TURNAROUND_COUNT ?= 6000
CFLAGS += -DRF_TURNAROUND_COUNT=$(RF_TURNAROUND_COUNT)
# Stock-interop fix: skip the redundant pre-response RF_SetChannel. The radio is
# already on the just-received poll channel, and re-setting it misses stock RX.
STOCK_RESP_NO_SETCH ?= 1
CFLAGS += -DSTOCK_RESP_NO_SETCH=$(STOCK_RESP_NO_SETCH)
# Stock-interop fix: pair-ACK type tag is the previous connected-hop seed; the
# first connected poll is seed+1. Override only for bench regressions.
STOCK_CONNECT_IDX_BIAS ?= 1
CFLAGS += -DSTOCK_CONNECT_IDX_BIAS=$(STOCK_CONNECT_IDX_BIAS)
# Additional bench-only interop probes (STOCK_SUPPRESS_RESPONSES,
# STOCK_PARK_AFTER_CATCH, STOCK_SEED_BOND, ...) default off via #ifndef
# fallbacks in src/rf_task.c; enable with EXTRA_CFLAGS=-DSTOCK_...=1.
# INT_SOFT=1 forces plain __attribute__((interrupt)) and is known to produce
# broken RF/BLE binaries against LIBCH59xBLE.a. Keep native
# WCH-Interrupt-fast/HPE semantics; do not emit a firmware image otherwise.
INT_SOFT ?= 0
ifneq ($(INT_SOFT),0)
$(error INT_SOFT=$(INT_SOFT) is disabled for OpenController; use WCH GCC12 with INT_SOFT=0)
endif

# Linker flags. -L $(BUILD) resolves ld/ch592f.ld's `INCLUDE memory.ld`
# against the per-slot generated MEMORY block.
LDFLAGS := $(ARCH) -T ld/ch592f.ld -L $(BUILD)
LDFLAGS += -nostartfiles
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,--print-memory-usage
LDFLAGS += -Wl,-Map=$(BUILD)/$(TARGET).map
LDFLAGS += --specs=nano.specs --specs=nosys.specs

# Include paths
INCLUDES := -I src
INCLUDES += -I $(BLE_LIB_DIR)
INCLUDES += -I $(SDK_BLE_DIR)/HAL/include
INCLUDES += -I $(SDK_SRC_DIR)/StdPeriphDriver/inc
INCLUDES += -I $(SDK_SRC_DIR)/RVMSIS
INCLUDES += -I $(OPENBOOT)/firmware/app

# Source files - our application
# This is the keyboard-side wireless module: no USB device. Firmware updates
# go through OpenBoot (A6 81 -> openboot_request_update; see BOOT.md).
APP_SRC := src/main.c
APP_SRC += src/rf_task.c
APP_SRC += src/keyboard_uart.c
APP_SRC += src/kbd_image_id.c
APP_SRC += $(OPENBOOT)/firmware/app/openboot_app.c

# Source files - SDK startup
STARTUP_SRC := src/startup_CH592_phased.S
# (was $(SDK)/SRC/Startup/startup_CH592.S — swapped to a phased fork that
# writes 0xC0..0xC5 SRAM markers at each reset phase. See header of
# src/startup_CH592_phased.S for the marker map. Vendor logic preserved
# byte-for-byte; only marker stores added.)

# Source files - SDK HAL (TMOS/BLE init)
HAL_SRC := $(SDK_BLE_DIR)/HAL/MCU.c
HAL_SRC += $(SDK_BLE_DIR)/HAL/RTC.c
HAL_SRC += $(SDK_BLE_DIR)/HAL/SLEEP.c
HAL_SRC += $(SDK_BLE_DIR)/HAL/KEY.c
HAL_SRC += $(SDK_BLE_DIR)/HAL/LED.c

# Source files - SDK peripheral drivers (only what we need)
DRV_SRC := $(SDK_SRC_DIR)/StdPeriphDriver/CH59x_clk.c
DRV_SRC += $(SDK_SRC_DIR)/StdPeriphDriver/CH59x_gpio.c
DRV_SRC += $(SDK_SRC_DIR)/StdPeriphDriver/CH59x_sys.c
DRV_SRC += $(SDK_SRC_DIR)/StdPeriphDriver/CH59x_uart1.c
DRV_SRC += $(SDK_SRC_DIR)/StdPeriphDriver/CH59x_flash.c
DRV_SRC += $(SDK_SRC_DIR)/StdPeriphDriver/CH59x_pwr.c
DRV_SRC += $(SDK_SRC_DIR)/StdPeriphDriver/CH59x_adc.c

# Source files - RISC-V core support
CORE_SRC := $(SDK_SRC_DIR)/RVMSIS/core_riscv.c

# Source files - BLE task scheduler. The CH59x BLE library ships a working
# ble_task_scheduler.S that dispatches the LLE (link-layer) interrupt through
# the g_LLE_IRQLibHandlerLocation function pointer. We link it for LLE.
SCHED_SRC := $(BLE_LIB_DIR)/ble_task_scheduler.S
# No app BB_IRQHandler: the BLE library fast-vectors the BLEB baseband IRQ in
# hardware. RF_RoleInit -> BLE_IPCoreInit -> ... -> BB_DevInit installs a
# Qingke-V4 PFIC fast-vector (FIADDRR[3] = BB_IRQLibFunction | 1) for IRQ 20,
# so BLEB jumps straight to the library handler, bypassing the software vector
# table. The old src/bb_irq_trampoline.S was therefore dead code -- bench-proven
# 2026-06-30 on both v1.00 and v1.4.2: with it present, bb_irq_count stayed 0
# while RF callbacks fired, and removing it kept full pairing + connected HID
# delivery. Matches the SDK LWNS example, which ships no app BB handler.
# (NOTE: no app-side PFIC_EnableIRQ(BLEB/BLEL) calls are required in main.c; the
# earlier contrary note was a debugger-reset artifact. See docs/TMOS_REVIEW.md.)
# Strong NMI_Handler / HardFault_Handler overrides. Vendor startup ships
# weak handlers that infinite-loop with zero diagnostic state. Ours
# capture mepc/mcause/mtval to fixed SRAM (0x20005804..0x20005810) so
# the post-mortem SWD read sees exactly where the fault hit. Required
# for diagnosing the 2026-05-17 dongle reset-on-RF-activity blocker.
SCHED_SRC += src/fault_handler.S

# Libraries
LIBS := $(BLE_LIB_DIR)/LIBCH59xBLE.a
LIBS += $(SDK_SRC_DIR)/StdPeriphDriver/libISP592.a

# Collect all sources
C_SRCS := $(APP_SRC) $(HAL_SRC) $(DRV_SRC) $(CORE_SRC)
S_SRCS := $(STARTUP_SRC) $(SCHED_SRC)

# Object files
C_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(C_SRCS)))
S_OBJS := $(patsubst %.S,$(BUILD)/%.o,$(notdir $(S_SRCS)))
OBJS   := $(C_OBJS) $(S_OBJS)

# Rebuild objects when command-line build flags change. Bench work relies on
# macros such as STOCK_SEED_BOND and RF_TURNAROUND_COUNT; without a flag stamp,
# make can reuse stale objects when only those values change. SLOT and the
# OpenBoot pin are part of the fingerprint: both reach the compiled bytes.
CONFIG_FINGERPRINT := $(shell printf '%s\n' "CC=$(CC)" "AS=$(AS)" "CFLAGS=$(CFLAGS)" "INCLUDES=$(INCLUDES)" "SLOT=$(SLOT)" "OPENBOOT=$(OPENBOOT_REVISION)" "TOOLCHAIN=$(MRS_TOOLCHAIN)" "CCVER=$(shell "$(MRS_TOOLCHAIN)/$(CC)" -dumpfullversion 2>/dev/null)" | sha256sum | cut -d' ' -f1)
CONFIG_STAMP := $(BUILD)/.config-$(CONFIG_FINGERPRINT)

# VPATH for source file lookup
VPATH := src
VPATH += $(SDK_BLE_DIR)/HAL
VPATH += $(SDK_SRC_DIR)/StdPeriphDriver
VPATH += $(SDK_SRC_DIR)/RVMSIS
VPATH += $(BLE_LIB_DIR)
VPATH += $(OPENBOOT)/firmware/app

# Maximum app size: the OpenBoot slot CAPACITY (slot size minus the 4 KB
# OBR2 record block). Derived from the submodule, never a literal.
APP_MAX_BYTES := $(OPENBOOT_SLOT_CAP)

MINICHLINK ?= minichlink
GDB        := $(PREFIX)gdb
# WCH-LinkE probe serial. Empty selects whatever single probe is attached;
# set KBD_PROBE=<serial> when multiple probes are connected. Destructive
# targets (flash-factory) refuse to run without it.
KBD_PROBE ?=
PROBE_ARG := $(if $(KBD_PROBE),-l $(KBD_PROBE))

.PHONY: all clean size size-check info gdbserver debug factory flash-factory \
        bundle update check-deps check-openboot-toolchain check-factory-slot \
        openboot-image openboot-test FORCE

all: $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin size size-check

$(CONFIG_STAMP): | $(BUILD)
	@touch $@

$(OBJS): $(CONFIG_STAMP)

# The MEMORY block, generated per slot so ld/ch592f.ld holds no literal
# addresses. FORCE plus a content guard: it re-evaluates on every build (the
# geometry can move when the submodule pin does) but only rewrites the file
# when the bytes actually change, so an unchanged slot does not relink.
$(MEMORY_LD): FORCE | $(BUILD)
	@ram_len=$$(printf '0x%X' $$(( $(OPENBOOT_BOOTREQ_ADDR) - 0x20000000 ))); \
	 { echo "/* Generated by firmware/Makefile for slot $(SLOT). DO NOT EDIT. */"; \
	   echo "MEMORY"; \
	   echo "{"; \
	   echo "    FLASH (rx)  : ORIGIN = $(OPENBOOT_SLOT_BASE), LENGTH = $(OPENBOOT_SLOT_CAP)"; \
	   echo "    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = $$ram_len"; \
	   echo "}"; \
	   echo "OC_SLOT_A_BASE = $(OPENBOOT_SLOT_A_BASE);"; \
	   echo "OC_SLOT_B_BASE = $(OPENBOOT_SLOT_B_BASE);"; \
	   echo "OC_BOOTREQ_ADDR = $(OPENBOOT_BOOTREQ_ADDR);"; \
	 } > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv -f "$@.tmp" "$@"; fi

FORCE:

$(BUILD)/$(TARGET).elf: $(OBJS) $(LIBS) ld/ch592f.ld $(MEMORY_LD)
	$(LD) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: %.S | $(BUILD)
	$(AS) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

size: $(BUILD)/$(TARGET).elf
	$(SIZE) $<

size-check: $(BUILD)/$(TARGET).bin
	@sz=$$(stat -c%s $<); max=$$(( $(APP_MAX_BYTES) )); \
	 pct=$$(( sz * 100 / max )); \
	 if [ $$sz -gt $$max ]; then \
	    echo "ERROR: firmware is $$sz B, exceeds the $$max B slot capacity."; \
	    echo "       The top 4 KB block of each slot belongs to OpenBoot's"; \
	    echo "       OBR2 boot record; an image reaching into it is destroyed"; \
	    echo "       by the first COMMIT."; \
	    exit 1; \
	 else \
	    echo "  firmware: $$sz / $$max B ($$pct% of slot capacity, slot $(SLOT))"; \
	 fi

# ---------------------------------------------------------------------------
# Dependency / toolchain gates
# ---------------------------------------------------------------------------
# Assert the OpenBoot submodule IS the pinned revision and its worktree is
# clean. --skip-toolchain because verifying a SHA must not require GCC12 to
# be installed. (The WCH SDK is vendored in-tree - see vendor/README.md - so
# there is no SDK-revision half to this check.)
check-deps:
	$(PYTHON) -I $(OPENBOOT)/firmware/check_dependencies.py \
	    --expect-revision $(OPENBOOT_REVISION) --skip-toolchain

# OpenBoot's own compiler gate is ADVISORY: a mismatch warns and continues.
# GCC15 is bench-unvalidated for the bootloader (upstream measured erratic
# idle auto-boot at 1.5-4.8 s against a configured 10 s, and 4/32 failed
# software-reset returns on CH572). Keep a HARD gate on the paths that build
# the bootloader. Override only to reproduce that investigation:
#   make ... OPENBOOT_ALLOW_UNVALIDATED_GCC=1
check-openboot-toolchain:
	@if [ "$(OPENBOOT_ALLOW_UNVALIDATED_GCC)" = "1" ]; then \
	  echo "openboot toolchain: check bypassed (OPENBOOT_ALLOW_UNVALIDATED_GCC=1)"; \
	  exit 0; \
	fi; \
	gcc=""; \
	for p in riscv-wch-elf-gcc riscv32-wch-elf-gcc; do \
	  if [ -x "$(OPENBOOT_TOOLCHAIN)/$$p" ]; then \
	    gcc="$(OPENBOOT_TOOLCHAIN)/$$p"; break; \
	  fi; \
	done; \
	test -n "$$gcc" || { \
	  echo "no riscv-wch-elf-gcc or riscv32-wch-elf-gcc under OPENBOOT_TOOLCHAIN='$(OPENBOOT_TOOLCHAIN)'" >&2; \
	  exit 1; }; \
	major=$$("$$gcc" -dumpversion | cut -d. -f1); \
	test "$$major" = "12" || { \
	  echo "OpenBoot must be built with GCC 12; $$gcc is GCC $$major" >&2; \
	  echo "  Point OPENBOOT_TOOLCHAIN at a GCC12 bin directory, or set" >&2; \
	  echo "  OPENBOOT_ALLOW_UNVALIDATED_GCC=1 to reproduce the GCC15" >&2; \
	  echo "  investigation deliberately." >&2; \
	  exit 1; }; \
	echo "openboot toolchain: GCC $$major ($$gcc)"

# Standalone bootloader image (SWD recovery / inspection).
openboot-image: check-openboot-toolchain
	+$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch592 TRANSPORT=uart \
	    BOARD=$(OPENBOOT_BOARD) MRS_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" image

# OpenBoot's own gates at the pinned revision (protocol consts, host-native
# core tests, Rust fmt/clippy/tests) - run before moving the pin.
openboot-test:
	+$(MAKE) -C $(OPENBOOT)/firmware test
	cd $(OPENBOOT)/tools && cargo fmt --check && \
	    cargo clippy --all-targets -- -D warnings && cargo test

# ---------------------------------------------------------------------------
# Factory image (bootloader + blessed slot-A app), whole-chip @0
# ---------------------------------------------------------------------------
# Factory composition lives in OpenBoot: the image is
#   bootloader || 0x00 pad || app || zeros || slot A's OBR2 boot record
# so a blank part boots the application on FIRST POWER-ON with no host and no
# `openboot bless`. The 0x00 pad matters: programming 0xFF programs nothing
# on CH5xx, so an 0xFF pad would never land and the readback compare below
# would fail across it. Never recompose this locally.
#
# check-factory-slot sits on the composing rule itself, not just the phony
# target: asking for the factory .bin directly must not compose a slot-B app
# into a slot-A image.
check-factory-slot:
	@test "$(SLOT)" = "A" || { \
	  echo "factory images compose the SLOT=A build only (the blessed record describes slot A); use 'make bundle' for updates" >&2; \
	  exit 1; }

$(FACTORY_BIN) $(FACTORY_MANIFEST) &: $(BUILD)/$(TARGET).bin \
        | $(BUILD) check-openboot-toolchain check-factory-slot
	+$(MAKE) -C $(OPENBOOT)/firmware CHIP=ch592 TRANSPORT=uart \
	    BOARD=$(OPENBOOT_BOARD) MRS_TOOLCHAIN="$(OPENBOOT_TOOLCHAIN)" \
	    FACTORY_BLESS=1 APP="$(abspath $(BUILD)/$(TARGET).bin)" factory
#	The manifest is written by the same OpenBoot build that produced these
#	bytes; checking it here (not only in check-deps) closes the parallel-make
#	hole where check-deps finishes after the submodule was read.
	@rev=$$(sed -n 's/^openboot_revision=//p' "$(OPENBOOT_MANIFEST)"); \
	 dirty=$$(sed -n 's/^openboot_dirty=//p' "$(OPENBOOT_MANIFEST)"); \
	 test "$$rev" = "$(OPENBOOT_REVISION)" || { \
	   echo "OpenBoot built from $$rev, expected $(OPENBOOT_REVISION)" >&2; exit 1; }; \
	 test "$$dirty" = "0" || { \
	   echo "OpenBoot worktree is dirty; the image is not the pinned revision's bytes" >&2; \
	   exit 1; }
#	Size is a cheap, decisive check that the blessed record is present:
#	slot base + slot capacity + one record. A short image means FACTORY_BLESS
#	did not take effect and the unit would sit dead in the bootloader.
	@want=$$(( $(OPENBOOT_SLOT_A_BASE) + $(OPENBOOT_SLOT_CAP) + $(OPENBOOT_RECORD_SZ) )); \
	 got=$$(stat -c%s "$(OPENBOOT_FACTORY_BIN)"); \
	 test "$$got" -eq "$$want" || { \
	   echo "factory image is $$got B, expected $$want B (blessed record missing?)" >&2; \
	   exit 1; }
	@cp "$(OPENBOOT_FACTORY_BIN)" "$(FACTORY_BIN)"
	@cp "$(OPENBOOT_MANIFEST)" "$(FACTORY_MANIFEST)"
	@echo "factory image: $$(stat -c%s "$(FACTORY_BIN)") B (blessed, slot A)"

factory: $(FACTORY_BIN)

# Whole-chip factory install over SWD. Erases the code flash (NOT DataFlash:
# the KBD2 bond at physical 0x74000 survives -E), writes the factory image at
# 0, verifies by READBACK (minichlink ignores its own CH5xx erase/write return
# values and its verify is disabled), then does a REAL power cycle: -b reboots
# out of halt without re-running the boot path, so it would prove nothing
# about first-power-on behavior.
flash-factory: factory
	@command -v "$(MINICHLINK)" >/dev/null 2>&1 || { \
	  echo "minichlink not found: $(MINICHLINK)" >&2; exit 1; }
	@test -n "$(strip $(KBD_PROBE))" || { \
	  echo "set KBD_PROBE=<probe serial> - flash-factory refuses to guess the target (a multi-probe bench includes devices that must never be written)" >&2; \
	  exit 1; }
#	The 2.4G bond ("KBD2" at physical 0x74000) lives in DataFlash, which -E
#	does not touch - so check it FIRST and refuse before erasing anything: a
#	shipped unit must not carry a test fixture's pairing identity, and a
#	refused unit should be left unmodified. A development/bench reflash
#	keeps the bond deliberately via ALLOW_BONDED_FLASH=1 (compared against
#	exactly "1" so a scripted =0 fails safe). Clear it over UART (A6 52
#	unpair) if needed - never write CH592 DataFlash over SWD.
	"$(MINICHLINK)" -C linke -r "$(BUILD)/.flash-bond.bin" 0x74000 4 $(PROBE_ARG)
	@if head -c 4 "$(BUILD)/.flash-bond.bin" | od -An -tx1 | grep -qi "4b 42 44 32"; then \
	  if [ "$(strip $(ALLOW_BONDED_FLASH))" = "1" ]; then \
	    echo "WARNING: KBD2 bond present at 0x74000 (-E will not clear DataFlash); continuing (ALLOW_BONDED_FLASH=1)" >&2; \
	  else \
	    echo "ERROR: a KBD2 bond is present at 0x74000 - this unit carries a pairing identity" >&2; \
	    echo "       that would survive the factory flash. Clear it over UART (A6 52 unpair)," >&2; \
	    echo "       or pass ALLOW_BONDED_FLASH=1 to reflash a development unit deliberately." >&2; \
	    echo "       Nothing has been erased or written." >&2; \
	    exit 1; \
	  fi; fi
	"$(MINICHLINK)" -kt -C linke $(PROBE_ARG)
	"$(MINICHLINK)" -E -C linke $(PROBE_ARG)
	"$(MINICHLINK)" -C linke -w "$(abspath $(FACTORY_BIN))" 0 $(PROBE_ARG)
	"$(MINICHLINK)" -C linke -r "$(BUILD)/.flash-readback.bin" 0 $$(stat -c%s "$(FACTORY_BIN)") $(PROBE_ARG)
	cmp "$(BUILD)/.flash-readback.bin" "$(FACTORY_BIN)"
	@echo "factory image read back OK"
	"$(MINICHLINK)" -kt -C linke $(PROBE_ARG)
	"$(MINICHLINK)" -k3 -C linke $(PROBE_ARG)
	@echo "NOTE: image is factory-blessed (slot A record included) - the unit"
	@echo "      boots the application on first power-on; no bless step."

# ---------------------------------------------------------------------------
# A/B update bundle + bench OBP update over UART
# ---------------------------------------------------------------------------
# Both slots plus the bundle. Recursive because SLOT selects the object tree,
# the link base, and the -D values, so the two builds cannot share a pass.
#
# CONSTRAINT: do not combine bundle/factory/update goals in one parallel
# invocation (e.g. `make -j bundle factory`). The recursive makes here and in
# the factory rule do not coordinate target ownership with each other, and
# both recurse into the shared OpenBoot submodule build directory - run such
# goals as separate invocations.
bundle:
	+$(MAKE) SLOT=A $(SLOT_A_BIN)
	+$(MAKE) SLOT=B $(SLOT_B_BIN)
	@test -x "$(OPENBOOT_CLI)" || { \
	  echo "build the OpenBoot CLI first: cargo build --release --manifest-path $(OPENBOOT)/tools/Cargo.toml" >&2; \
	  exit 1; }
	"$(OPENBOOT_CLI)" bundle create -o "$(BUNDLE_BIN)" --chip ch592 \
	    "$(SLOT_A_BIN)@$(OPENBOOT_SLOT_A_BASE)" \
	    "$(SLOT_B_BIN)@$(OPENBOOT_SLOT_B_BASE)"
	@echo "bundle: $(abspath $(BUNDLE_BIN)) ($$(stat -c%s "$(BUNDLE_BIN)") B)"

# Bench OBP update over the WCH-LinkE CDC wired to PB12/PB13. Opening the
# port resets the target into OpenBoot's 10 s idle window; the CLI's HELLO
# then holds it (fail-stay). Both bundle members are family-checked (ODG2
# family 0xB2 at image offset 0x25) before anything talks to the device:
# OpenBoot's COMMIT attests only length+CRC, so this is the only guard
# against cross-flashing a dongle CH592 image on a shared bench.
OB_PORT ?=
update: bundle
	@test -n "$(strip $(OB_PORT))" || { \
	  echo "set OB_PORT=/dev/serial/by-id/<keyboard probe CDC>" >&2; exit 1; }
	@for f in "$(SLOT_A_BIN)" "$(SLOT_B_BIN)"; do \
	  magic=$$(od -An -c -j $$(( 0x20 )) -N4 "$$f" | tr -d ' '); \
	  test "$$magic" = "ODG2" || { \
	    echo "ERROR: $$f carries no ODG2 identity header at offset 0x20" >&2; \
	    exit 1; }; \
	  fam=$$(od -An -tx1 -j $$(( 0x25 )) -N1 "$$f" | tr -d ' '); \
	  test "$$fam" = "b2" || { \
	    echo "ERROR: $$f has ODG2 family 0x$$fam, expected 0xb2 (OpenController keyboard)" >&2; \
	    exit 1; }; \
	done
#	Ask the running application to enter the bootloader (A6 81 = stock OTA
#	mode), then ride out the ~2 s of post-reset UART turbulence before the
#	first HELLO - the CLI's fast retries all land inside it otherwise
#	(bench-measured: HELLO fails at t<2 s, succeeds from ~2.3 s). If the
#	device is already in the bootloader the A6 81 bytes are SOF-hunt noise
#	and the settle is harmless.
	@$(PYTHON) -c 'import serial, sys, time; p = serial.Serial(sys.argv[1], 115200, timeout=0.2); time.sleep(1.0); p.write(bytes([0xA6, 0x81, 0x27])); time.sleep(6.0); p.close()' "$(OB_PORT)"
#	No separate `boot` step: the flash flow ends with BOOT itself
#	("boot after: yes"), so the device is already running the new image
#	when this returns - a trailing boot would HELLO an application.
	"$(OPENBOOT_CLI)" --transport uart --port "$(OB_PORT)" flash "$(BUNDLE_BIN)" --force

# Probe the keyboard-side CH592F.
# minichlink selects the probe with `-l <serial>`, which MUST be the last arg.
info:
	$(MINICHLINK) -i $(PROBE_ARG)

# -G "must be last", and so does -l; select the probe via the env var instead.
gdbserver:
	$(if $(KBD_PROBE),MINICHLINK_LINKE_SERIAL=$(KBD_PROBE) )$(MINICHLINK) -G

debug: $(BUILD)/$(TARGET).elf
	$(GDB) $< \
	    -ex "target extended-remote :2000" \
	    -ex "monitor reset halt" \
	    -ex "load" \
	    -ex "b main"

clean:
	rm -rf build
