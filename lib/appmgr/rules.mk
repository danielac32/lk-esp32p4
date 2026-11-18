LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/appmgr.c

GLOBAL_INCLUDES += $(LOCAL_DIR)/include

MODULE_DEPS += \
	platform

include make/module.mk
