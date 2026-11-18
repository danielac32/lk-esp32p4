LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/locktest.c

MODULE_DEPS += \
	app \
	kernel \
	platform

include make/module.mk
