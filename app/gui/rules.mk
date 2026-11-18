LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/gui.c

MODULE_DEPS += \
	app \
	kernel \
	lib/gfx \
	lib/font \
	lib/fs \
	app/netcfg \
	platform

include make/module.mk
