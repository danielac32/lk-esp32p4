# LK project for ESP32-P4 (SMP + SD/FAT + WiFi ESP-Hosted)

LOCAL_DIR := $(GET_LOCAL_DIR)

TARGET := esp32p4

MODULES += \
	app \
	app/shell \
	app/locktest \
	app/httpd \
	app/telnetd \
	app/tftpd \
	app/udpserver \
	app/netcfg \
	app/gui \
	dev/uart/esp32p4 \
	dev/block/sdmmc-esp32p4 \
	dev/fbcon/esp32p4 \
	dev/gpio \
	dev/wifi/esp32p4 \
	lib/bcache \
	lib/bio \
	lib/cmdline \
	lib/console \
	lib/fs \
	lib/fs/fat \
	lib/libcpp \
	lib/lktl \
	lib/minip \
	lib/json \
	external/lib/aes \
	external/lib/mincrypt \
	lib/elfload \
	app/elf \
	lib/appmgr \
	lib/unittest
# libm NO se incluye: el toolchain rv32 (ilp32f) no trae __muldf3/__adddf3
# en libgcc y el port las fuentes double de fdlibm nunca enlazarian.

# Entrada de consola con cbuf (fix "escribir el comando dos veces"): el driver
# UART mete los RX por IRQ en console_input_cbuf en vez de leer FIFO directo.
GLOBAL_DEFINES += CONSOLE_HAS_INPUT_BUFFER=1 \
	PKTBUF_POOL_SIZE=32 PKTBUF_EXTRA_HEADERS=8 PKTBUF_ETH_RX_POOL_RESERVE=8

include project/target/esp32p4.mk
