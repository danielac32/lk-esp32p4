LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/httpd.c

MODULE_DEPS += \
	app \
	kernel \
	lib/minip \
	lib/fs \
	lib/console

include make/module.mk
