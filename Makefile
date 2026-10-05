# Builds LavaEmu.pdx: the LAVA VM (src/lava.c) and the frontend (src/main.c) in
# plain C, linked by the Playdate SDK's own makefile rules.
#
#   make             device + Simulator build
#   make simulator   Simulator only
#   make run         build and open in the Simulator
#   make games       copy the English games from ~/wqx_tl (WQX_TL=...) into games/
#   make host        host/liblava.dylib, the VM for the host-side tests
#   make check       lockstep test against lavaemu (needs ~/wqx_tl) + state round trip
#   make autotest    Simulator build that plays itself and writes screenshots

# On macOS the Simulator build links with Xcode's toolchain; the Command Line Tools
# linker can lag behind the installed SDK.
ifeq ($(shell uname -s),Darwin)
  XCODE ?= /Applications/Xcode.app/Contents/Developer
  ifneq ($(wildcard $(XCODE)),)
    export DEVELOPER_DIR := $(XCODE)
    export SDKROOT := $(XCODE)/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
  endif
endif

HEAP_SIZE      = 8388208
STACK_SIZE     = 61800

PRODUCT = LavaEmu.pdx

SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)
endif
ifeq ($(SDK),)
$(error SDK path not found; set ENV value PLAYDATE_SDK_PATH)
endif

VPATH += src
SRC = src/main.c src/lava.c src/profiles.c
UINCDIR = src

include $(SDK)/C_API/buildsupport/common.mk

# -Os: smaller code ran faster than -O2 on the device for bbk_playdate (PSRAM
# instruction fetch); the VM loop is the hot path either way.
OPT = -Os -falign-functions=16 -fomit-frame-pointer
# The Simulator build gets optimisation too (common.mk builds it with -g only),
# and UDEFS (e.g. -DLAVA_AUTOTEST), which common.mk passes only to device builds.
CLANGFLAGS = -g -O2
SIMCOMPILER += $(UDEFS)

# Device builds need newlib, which Homebrew's bare arm-none-eabi-gcc lacks. Prefer Arm's
# toolchain (brew install --cask gcc-arm-embedded), or point ARM_TOOLCHAIN at its bin/.
ARM_TOOLCHAIN ?= $(lastword $(sort $(wildcard /Applications/ArmGNUToolchain/*/arm-none-eabi/bin/)))
ifneq ($(ARM_TOOLCHAIN),)
  GCC := $(patsubst %//,%/,$(ARM_TOOLCHAIN)/)
  OJBCPY := $(GCC)
endif

# Games: games/ (gitignored, filled by `make games`) is copied into the pdx as
# Bundled/. Nothing from it is committed.
WQX_TL ?= $(HOME)/wqx_tl

.PHONY: bundled games host check autotest run FORCE

bundled:
	@mkdir -p games Source/Bundled
	rsync -a --delete --exclude '.*' games/ Source/Bundled/

device_bin simulator_bin: bundled

games:
	python3 tools/bundle.py --wqx-tl $(WQX_TL) $(if $(ORIGINALS),--originals) games

HOST_CC = $(if $(filter Darwin,$(shell uname -s)),xcrun cc,cc)
host/liblava.dylib: src/lava.c src/lava.h tools/lavahost.c
	@mkdir -p host
	$(HOST_CC) -O2 -Wall -Wextra -Wno-unused-parameter -shared -fPIC -o $@ src/lava.c tools/lavahost.c

host: host/liblava.dylib

check: host
	python3 tests/test_states.py
	python3 tests/lockstep.py

run: simulator
	open -a "$(SDK)/bin/Playdate Simulator.app" $(PRODUCT)

# Simulator build that drives itself (LAVA_AUTOTEST in src/main.c) and writes
# screens and timings to the Simulator's Data/com.gopherbone.lavaemu/autotest/.
autotest:
	$(MAKE) clean
	$(MAKE) simulator UDEFS=-DLAVA_AUTOTEST
	open -a "$(SDK)/bin/Playdate Simulator.app" $(PRODUCT)
