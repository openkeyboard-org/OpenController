# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 OpenController contributors
#
# OpenController keyboard wireless-module firmware — CH592F
# Target: WCH CH592F (QingKe RISC-V4F, 60MHz, 448KB flash, 26KB SRAM)
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

# Project
TARGET := opencontroller_ch592f
BUILD  := build
FLASHABLE := $(BUILD)/$(TARGET)_flashable.bin

# Architecture flags
ARCH   := -march=rv32imac_zicsr_zifencei -mabi=ilp32
ARCH   += -msmall-data-limit=8 -mno-save-restore

# Compiler flags
CFLAGS := $(ARCH) -O2 -g
CFLAGS += -std=gnu99
CFLAGS += -Wall -Wextra -Wno-unused-parameter
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -fno-common

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

# Linker flags
LDFLAGS := $(ARCH) -T ld/ch592f.ld
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

# Source files - our application
# This is the keyboard-side wireless module, so USB device + IAP are omitted.
APP_SRC := src/main.c
APP_SRC += src/rf_task.c
APP_SRC += src/keyboard_uart.c

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
# make can reuse stale objects when only those values change.
CONFIG_FINGERPRINT := $(shell printf '%s\n' "CC=$(CC)" "AS=$(AS)" "CFLAGS=$(CFLAGS)" "INCLUDES=$(INCLUDES)" | sha256sum | cut -d' ' -f1)
CONFIG_STAMP := $(BUILD)/.config-$(CONFIG_FINGERPRINT)

# VPATH for source file lookup
VPATH := src
VPATH += $(SDK_BLE_DIR)/HAL
VPATH += $(SDK_SRC_DIR)/StdPeriphDriver
VPATH += $(SDK_SRC_DIR)/RVMSIS
VPATH += $(BLE_LIB_DIR)

# Maximum app size — see link.ld. Anything larger overlaps the IAP staging
# region at 0x37000 (wiped on every flash) or the stage-1 code at 0x6D000
# (brickable). Keep below this or revisit the flash map.
APP_MAX_BYTES := 221184

MINICHLINK ?= minichlink
GDB        := $(PREFIX)gdb
# WCH-LinkE probe serial. Empty selects whatever single probe is attached;
# set KBD_PROBE=<serial> when multiple probes are connected.
KBD_PROBE ?=
PROBE_ARG := $(if $(KBD_PROBE),-l $(KBD_PROBE))

.PHONY: all clean size size-check info gdbserver debug flash-app flash-bare

all: $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin size size-check

$(CONFIG_STAMP): | $(BUILD)
	@touch $@

$(OBJS): $(CONFIG_STAMP)

$(BUILD)/$(TARGET).elf: $(OBJS) $(LIBS) ld/ch592f.ld
	$(LD) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(FLASHABLE): $(BUILD)/$(TARGET).bin | $(BUILD)
	{ perl -e 'print "\x6f\x10\x00\x00", "\xff" x 4092'; cat $<; } > $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: %.S | $(BUILD)
	$(AS) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

size: $(BUILD)/$(TARGET).elf
	$(SIZE) $<

size-check: $(BUILD)/$(TARGET).bin
	@sz=$$(stat -c%s $<); \
	 pct=$$(( sz * 100 / $(APP_MAX_BYTES) )); \
	 if [ $$sz -gt $(APP_MAX_BYTES) ]; then \
	    echo "ERROR: firmware is $$sz B, exceeds $(APP_MAX_BYTES) B budget ($$pct%)."; \
	    echo "       see link.ld comment — growing past this overlaps the IAP"; \
	    echo "       staging region or the vendor stage-1 code."; \
	    exit 1; \
	 else \
	    echo "  firmware: $$sz / $(APP_MAX_BYTES) B ($$pct% of app-flash budget)"; \
	 fi

# Probe the keyboard-side CH592F.
# minichlink selects the probe with `-l <serial>`, which MUST be the last arg.
info:
	$(MINICHLINK) -i $(PROBE_ARG)

# Flash only the app slot. The vendor 0x0000..0x0fff stage-1/trampoline must
# already be present on the target (stock-compatible boot chain layout).
flash-app: $(BUILD)/$(TARGET).bin
	$(MINICHLINK) -w $< 0x1000 $(PROBE_ARG)
	$(MINICHLINK) -b $(PROBE_ARG)

# Flash a bare CH592F fixture that has no vendor boot chain installed.
# The first page contains a 4-byte `jal x0, 0x1000` trampoline and erased
# padding; the app remains linked and placed at 0x1000.
flash-bare: $(FLASHABLE)
	$(MINICHLINK) -w $< 0x0 $(PROBE_ARG)
	$(MINICHLINK) -b $(PROBE_ARG)

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
	rm -rf $(BUILD)
