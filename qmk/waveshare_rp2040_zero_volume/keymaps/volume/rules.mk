SRC += font_store.c

# The repo's VERSION (four levels up, through the qmk_firmware symlink),
# reported to the host by VERSION_QUERY_CMD.
FW_VERSION := $(subst ., ,$(strip $(shell cat $(KEYMAP_PATH)/../../../../VERSION)))
OPT_DEFS += -DFW_VERSION_MAJOR=$(word 1,$(FW_VERSION)) -DFW_VERSION_MINOR=$(word 2,$(FW_VERSION)) -DFW_VERSION_PATCH=$(word 3,$(FW_VERSION))

# The store's slots start right after the flash's first FIRMWARE_MAX bytes
# (config.h), so a firmware grown past them would be overwritten by the
# first gif uploaded: fail the build instead.
FIRMWARE_MAX_BYTES := $(shell sed -n 's/^.define FIRMWARE_MAX \(0x[0-9A-Fa-f]*\).*/\1/p' $(KEYMAP_PATH)/../../config.h)
check-size: firmware-fits
firmware-fits: build
	@end=$$($(NM) $(BUILD_DIR)/$(TARGET).elf | sed -n 's/^\([0-9a-f]*\) . __flash_binary_end$$/\1/p'); \
	used=$$((0x$$end - 0x10000000)); max=$$(($(FIRMWARE_MAX_BYTES))); \
	if [ $$used -gt $$max ]; then echo "the firmware takes $$used bytes, over config.h's FIRMWARE_MAX ($$max)" >&2; exit 1; fi; \
	echo "Firmware: $$used of the $$max bytes before the store"
.PHONY: firmware-fits
