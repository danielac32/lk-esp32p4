LOCAL_DIR := $(GET_LOCAL_DIR)
MODULE := $(LOCAL_DIR)

MODULE_DEPS += lib/bio lib/fs lib/partition

MODULE_SRCS += \
	$(LOCAL_DIR)/sdmmc_bus.c \
	$(LOCAL_DIR)/sd_hw.c \
	$(LOCAL_DIR)/sd_card.c \
	$(LOCAL_DIR)/sd_bio.c

include make/module.mk
