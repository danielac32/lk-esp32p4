LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/netcfg.c \
	$(LOCAL_DIR)/autorun.c

GLOBAL_INCLUDES += $(LOCAL_DIR)/include

MODULE_DEPS += \
	app \
	kernel \
	lib/cmdline \
	lib/json \
	lib/fs \
	lib/minip

include make/module.mk
