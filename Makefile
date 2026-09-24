# Build and flash sketches for the STM32F411CE "Black Pill" via ST-Link.
#
# Each sketch is a directory holding a like-named .ino, per Arduino convention:
#
#     blink/blink.ino
#     serial-echo/serial-echo.ino
#
# New directories are picked up automatically -- no edits here.
#
#     make                  build every sketch
#     make blink            build one
#     make flash-blink      build, verify and flash one
#     make flash            flash $(SKETCH) (default: $(SKETCH))
#     make list             show discovered sketches
#     make probe            identify the attached ST-Link and chip
#     make clean            remove all build output

# usb=CDCgen routes the generic `Serial` over the USB-C port (/dev/ttyACM0)
# instead of a U(S)ART.
FQBN       := STMicroelectronics:stm32:GenF4:pnum=BLACKPILL_F411CE,usb=CDCgen
BUILD_DIR  := build
FLASH_ADDR := 0x8000000

# Initial stack pointer expected in the vector table: top of the F411CE's
# 128 KB SRAM. Guards against flashing an image built for another MCU.
EXPECTED_SP := 20020000

# A directory is a sketch only if it contains <dir>/<dir>.ino.
# Sketches may live at the repo root or under modules/.
dirname  = $(notdir $(patsubst %/,%,$1))
SKETCH_DIRS := $(sort $(foreach d,$(wildcard */) $(wildcard modules/*/), \
                 $(if $(wildcard $(d)$(call dirname,$(d)).ino),$(d))))
SKETCHES := $(foreach d,$(SKETCH_DIRS),$(call dirname,$(d)))
sketchdir = $(filter $(1)/ %/$(1)/,$(SKETCH_DIRS))

# Sketch used by the bare `flash` and `check` targets.
SKETCH ?= $(firstword $(SKETCHES))

.PHONY: all list probe clean flash check

all: $(SKETCHES)

# A sketch may ship a build-flags.txt with extra compiler.cpp.extra_flags
# (e.g. library configuration macros normally set via a global header), a
# compat.h force-included ahead of every translation unit (e.g. to patch
# missing declarations in a third-party library without editing it), and/or
# a compat_includes/ directory added to the include search path (e.g. to
# shim a missing/case-mismatched header a library expects).
extra_flags = $(strip \
  $(if $(wildcard $(call sketchdir,$(1))build-flags.txt),$(file <$(call sketchdir,$(1))build-flags.txt)) \
  $(if $(wildcard $(call sketchdir,$(1))compat.h),-include $(abspath $(call sketchdir,$(1))compat.h)) \
  $(if $(wildcard $(call sketchdir,$(1))compat_includes/),-I$(abspath $(call sketchdir,$(1))compat_includes)))

# Per-sketch build, check and flash targets.
define SKETCH_RULES
$(BUILD_DIR)/$(1)/$(1).ino.bin: $(wildcard $(call sketchdir,$(1))*.ino $(call sketchdir,$(1))*.h $(call sketchdir,$(1))*.cpp $(call sketchdir,$(1))*.c $(call sketchdir,$(1))build-flags.txt)
	arduino-cli compile -b "$(FQBN)" --output-dir $(BUILD_DIR)/$(1) $(if $(call extra_flags,$(1)),--build-property "compiler.cpp.extra_flags=$(call extra_flags,$(1))") $(call sketchdir,$(1))

.PHONY: $(1) check-$(1) flash-$(1)

$(1): $(BUILD_DIR)/$(1)/$(1).ino.bin

check-$(1): $(BUILD_DIR)/$(1)/$(1).ino.bin
	@./tools/check-sp.sh $$< $(EXPECTED_SP)

flash-$(1): check-$(1)
	st-flash --reset write $(BUILD_DIR)/$(1)/$(1).ino.bin $(FLASH_ADDR)
endef

$(foreach s,$(SKETCHES),$(eval $(call SKETCH_RULES,$(s))))

# Convenience aliases for the default sketch.
check: check-$(SKETCH)
flash: flash-$(SKETCH)

list:
	@printf '%s\n' $(SKETCHES) | sed 's/^/  /'
	@echo "default (SKETCH): $(SKETCH)"

probe:
	st-info --probe

clean:
	rm -rf $(BUILD_DIR)
