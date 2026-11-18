LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/elfload.c \
	$(LOCAL_DIR)/exports.c

GLOBAL_INCLUDES += $(LOCAL_DIR)/include

MODULE_DEPS += \
	lib/fs \
	lib/appmgr \
	platform

include make/module.mk
