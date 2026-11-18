LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += $(LOCAL_DIR)/uart.c

MODULE_DEPS += \
	lib/cbuf \
	lib/io \
	platform

include make/module.mk
