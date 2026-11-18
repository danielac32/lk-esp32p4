LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

PLATFORM := esp32p4
TARGET := esp32p4

# Use custom ESP32-P4 linker script (XIP flash + L2MEM data)
LINKER_SCRIPT := platform/esp32p4/linker.ld

MODULE_SRCS += $(LOCAL_DIR)/init.c


include make/module.mk

