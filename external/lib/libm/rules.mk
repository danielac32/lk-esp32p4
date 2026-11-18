LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_CFLAGS += -Wno-unused-variable -Wno-sign-compare -Wno-parentheses -Wno-double-promotion
ifeq ($(call is_warning_flag_supported,-Wmaybe-uninitialized),yes)
MODULE_CFLAGS += -Wno-maybe-uninitialized
endif
MODULE_OPTIONS := float

# ESP32-P4 compila con -mabi=ilp32f (FPU single precision).  Las variantes
# double de fdlibm requiren __muldf3/__adddf3 de libgcc, que no se enlazan en
# este port (usamos compiler_rt propio).  Solo se incluyen las variantes float
# y sus kernels, que operan 100% en single precision.
MODULE_FLOAT_SRCS += \
	$(LOCAL_DIR)/k_sinf.c \
	$(LOCAL_DIR)/k_cosf.c \
	$(LOCAL_DIR)/k_tanf.c \
	$(LOCAL_DIR)/e_rem_pio2f.c \
	$(LOCAL_DIR)/s_sinf.c \
	$(LOCAL_DIR)/s_cosf.c \
	$(LOCAL_DIR)/s_tanf.c \
	$(LOCAL_DIR)/e_sqrtf.c \
	$(LOCAL_DIR)/e_asinf.c \
	$(LOCAL_DIR)/e_acosf.c \
	$(LOCAL_DIR)/e_powf.c \
	$(LOCAL_DIR)/s_floorf.c \
	$(LOCAL_DIR)/s_ceilf.c \
	$(LOCAL_DIR)/s_scalbnf.c \
	$(LOCAL_DIR)/s_copysignf.c \
	$(LOCAL_DIR)/s_fabsf.c

include make/module.mk
