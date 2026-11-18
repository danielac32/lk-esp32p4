LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

ARCH := riscv
SUBARCH ?= 32
RISCV_MODE ?= machine
WITH_SMP ?= true
SMP_MAX_CPUS ?= 2
LK_HEAP_IMPLEMENTATION ?= dlmalloc
RISCV_FPU ?= true
RISCV_MMU ?= none

# ESP32-P4: rv32imafc_zicsr_zifencei, ilp32f (as per Xinu port)
ARCH_RISCV_EMBEDDED ?= false

MODULE_DEPS += lib/cbuf

MODULE_COMPILEFLAGS += -I$(LKROOT)/dev/include -I$(LKROOT)/dev/uart/esp32p4/include
GLOBAL_INCLUDES += \
	$(LKROOT)/dev/include \
	$(LKROOT)/dev/uart/esp32p4/include


# Kernel in internal L2MEM for data/bss/heap; code runs from flash XIP.
MEMBASE ?= 0x4FF00000
MEMSIZE ?= 0x000C0000 # 768KB L2MEM (TODO: evaluar reserva l2reserve como Xinu)

# ESP32-P4 has CLINT-compatible mtime/mtimecmp at 0x20000000.
# Interrupt controller is CLIC (hw-vectored).
PLATFORM_USE_CUSTOM_LINKER := 1

GLOBAL_DEFINES += \
	PLATFORM_SUPPORTS_PANIC_SHELL=1 \
	RISCV_NO_CACHE_OPS=1 \
	TARGET_SOC_ESP32P4=1 \
	PLATFORM_ESP32P4=1 \
	ARCH_RISCV_CLINT_BASE=0x20000000 \
	ARCH_RISCV_MTIME_RATE=400000000

MODULE_SRCS += \
	$(LOCAL_DIR)/start.S \
	$(LOCAL_DIR)/platform.c \
	$(LOCAL_DIR)/debug.c \
	$(LOCAL_DIR)/gpio.c \
	$(LOCAL_DIR)/i2c.c \
	$(LOCAL_DIR)/power.c \
	$(LOCAL_DIR)/touch.c \
	$(LOCAL_DIR)/es8311.c \
	$(LOCAL_DIR)/ahbdma.c \
	$(LOCAL_DIR)/i2s.c \
	$(LOCAL_DIR)/audio.c \
	$(LOCAL_DIR)/lcd.c \
	$(LOCAL_DIR)/compiler_rt.c \
	$(LOCAL_DIR)/clic.c \
	$(LOCAL_DIR)/clic_asm.S \
	$(LOCAL_DIR)/psram_heap.c \
	$(LOCAL_DIR)/shell_cmds.c

# RISC-V toolchain flags tuned for ESP32-P4
ARCH_COMPILEFLAGS += -march=rv32imafc_zicsr_zifencei -mabi=ilp32f

include make/module.mk
