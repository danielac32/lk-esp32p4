LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/udpserver.c

MODULE_DEPS += \
	app \
	kernel \
	lib/minip \
	lib/fs

include make/module.mk
