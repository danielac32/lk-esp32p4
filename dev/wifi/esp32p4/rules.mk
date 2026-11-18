LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/sdio_probe.c \
	$(LOCAL_DIR)/esp_hosted.c \
	$(LOCAL_DIR)/esp_hosted_sdio_lk.c \
	$(LOCAL_DIR)/esp_hosted_ctrl.c \
	$(LOCAL_DIR)/wifi_ctrl.c \
	$(LOCAL_DIR)/wifi_scan.c \
	$(LOCAL_DIR)/wifi_netif.c \
	$(LOCAL_DIR)/wifi_shell.c

MODULE_DEPS += \
	dev/block/sdmmc-esp32p4

MODULE_COMPILEFLAGS += -I$(LOCAL_DIR)/include -I$(LKROOT)/dev/block/sdmmc-esp32p4 -I$(LKROOT)/platform/esp32p4/include \
	-DESP_HOSTED_RX_QUEUE_DEPTH=4 -DESP_HOSTED_RX_PAYLOAD_MAX=1600


include make/module.mk
