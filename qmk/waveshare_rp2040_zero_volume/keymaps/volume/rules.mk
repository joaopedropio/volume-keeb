SRC += font_store.c

# The repo's VERSION (four levels up, through the qmk_firmware symlink),
# reported to the host by VERSION_QUERY_CMD.
FW_VERSION := $(subst ., ,$(strip $(shell cat $(KEYMAP_PATH)/../../../../VERSION)))
OPT_DEFS += -DFW_VERSION_MAJOR=$(word 1,$(FW_VERSION)) -DFW_VERSION_MINOR=$(word 2,$(FW_VERSION)) -DFW_VERSION_PATCH=$(word 3,$(FW_VERSION))
