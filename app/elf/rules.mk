LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/elfcmd.c

MODULE_DEPS += \
	app \
	lib/console \
	lib/elfload \
	lib/fs

include make/module.mk
