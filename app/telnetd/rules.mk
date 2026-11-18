LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/telnetd.c

MODULE_DEPS += \
	app \
	kernel \
	lib/minip \
	lib/console \
	lib/io

include make/module.mk
