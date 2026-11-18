LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/fbcon_p4.c

GLOBAL_INCLUDES += $(LOCAL_DIR)/include

MODULE_DEPS += \
	dev/fbcon \
	platform \
	lib/io \
	lib/libc \
	kernel

include make/module.mk
