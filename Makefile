# =============================================================================
# alchemy-lab-duet — Daisy-bootloader firmware for Hermetic Modular Alchemy Lab
# Built on Hermetic Modular's alchemy-template (MIT), whose Makefile this is.
#
# Standard Daisy workflow (libDaisy core Makefile underneath):
#   make libdaisy                   — build lib/libDaisy once after cloning
#   make PANEL_I2C_ADDR=0x42        — build for the first lab (0x43 for the second)
#   make program-dfu                — flash over USB (module in DFU mode; see README)
#   make clean                      — remove the build tree
# =============================================================================

# ── Which firmware to build ─────────────────────────────────────────────────
# One folder per firmware under src/, named after it (the same layout the
# Alchemy SDK's own examples/ use).  Switch with `make FW=<name>`; the build
# tree is wiped automatically when FW or BOARD changes, so stale objects from
# the previous firmware can never be linked in.
FW ?= duet
ifeq ($(wildcard src/$(FW)),)
$(error no firmware 'src/$(FW)' — available: $(notdir $(wildcard src/*)))
endif

TARGET = $(FW)

# Alchemy Lab board revision: v1 | v2
BOARD ?= v2
ifeq ($(filter $(BOARD),v1 v2),)
$(error BOARD must be 'v1' or 'v2' (got '$(BOARD)'))
endif

ALCHEMY_DIR  = lib/alchemy-sdk
LIBDAISY_DIR = lib/libDaisy

# ── App sources — every .cpp in the selected firmware's folder ──────────────
CPP_SOURCES = $(sort $(wildcard src/$(FW)/*.cpp))

# ── Shared app code, built into every firmware ──────────────────────────────
# src/common/ holds code that is not firmware-specific (the panel-display
# link).  Basenames must stay unique across the whole build — objects are
# flattened into build/ by basename — so do not prefix these with a firmware
# name and do not reuse a name from any src/<fw>/ folder.
CPP_SOURCES += $(sort $(wildcard src/common/*.cpp))

# ── Alchemy SDK, compiled straight from the submodule ───────────────────────
CPP_SOURCES += $(sort $(shell find $(ALCHEMY_DIR)/framework/src -name '*.cpp'))
CPP_SOURCES += $(sort $(wildcard $(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/src/*.cpp))

C_INCLUDES += \
    -Isrc \
    -Isrc/$(FW) \
    -I$(ALCHEMY_DIR)/framework/include \
    -I$(ALCHEMY_DIR)/hardware/include \
    -I$(ALCHEMY_DIR)/hardware/alchemy-lab/$(BOARD)/include

ifeq ($(BOARD),v2)
C_DEFS += -DALCHEMY_BOARD_V2
endif

# ── I2C address for the TFT panel link (src/common/panel_i2c) ───────────────
# Every module on the TFT's I2C bus needs its own address.  The Mega receiver
# (mega/duet) polls 0x42, 0x43 and 0x44:
#     make FW=<name> PANEL_I2C_ADDR=0x43
# Harmless for a firmware that does not use panel_i2c.
PANEL_I2C_ADDR ?= 0x42
ifeq ($(filter $(PANEL_I2C_ADDR),0x42 0x43 0x44),)
$(warning PANEL_I2C_ADDR=$(PANEL_I2C_ADDR) is not one the Mega receiver polls (0x42 0x43 0x44))
endif
C_DEFS += -DPANEL_I2C_ADDR=$(PANEL_I2C_ADDR)

# ── Daisy bootloader build (BOOT_SRAM) ──────────────────────────────────────
APP_TYPE = BOOT_SRAM
LDSCRIPT = $(ALCHEMY_DIR)/cmake/linkers/alchemy_stm32h750ib_sram.lds

# The Alchemy SDK requires C++17 (libDaisy's default is gnu++14).
CPP_STANDARD = -std=gnu++17

# ── libDaisy core Makefile does the rest ────────────────────────────────────
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Object files are flattened into build/ by basename, so switching FW or
# BOARD must not reuse the previous build tree.  The stamp names both, and the
# panel address too: it is a -D, which make cannot see change on its own.
BUILD_STAMP := $(BUILD_DIR)/.build-$(BOARD)-$(FW)-$(PANEL_I2C_ADDR)
ifeq ($(wildcard $(BUILD_STAMP)),)
_BUILD_GUARD := $(shell rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/*.d $(BUILD_DIR)/*.lst $(BUILD_DIR)/.board-* $(BUILD_DIR)/.build-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(BUILD_STAMP))
endif

.PHONY: list
list:
	@echo "firmware in src/: $(notdir $(wildcard src/*))"
	@echo "building: $(FW) (board $(BOARD)) -> $(BUILD_DIR)/$(TARGET).bin"

.PHONY: libdaisy
libdaisy:
	$(MAKE) -C $(LIBDAISY_DIR)

# ── Flash without touching the module ───────────────────────────────────────
# HostLink reboots the running module into the system bootloader over the
# same USB connection the web editor uses, then dfu-util (-w waits for the
# DFU device to enumerate) writes the app.
USBPID ?= df11

.PHONY: program-live
program-live: all
	node $(ALCHEMY_DIR)/tools/hostlink-cli/hostlink.mjs reboot bootloader
	dfu-util -w -a 0 -s $(FLASH_ADDRESS):leave -D $(BUILD_DIR)/$(TARGET_BIN) -d ,0483:$(USBPID)
