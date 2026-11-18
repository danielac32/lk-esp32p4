# Configuracion comun para apps ELF de LK (ET_REL, ld -r)

CC := riscv32-unknown-elf-gcc
LD := riscv32-unknown-elf-ld

LKSDK ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

CFLAGS = -Os -fno-builtin -fno-pic -ffunction-sections -std=gnu11 \
         -march=rv32imaf_zicsr_zifencei -mabi=ilp32f -mno-relax \
         -msmall-data-limit=0 -Wall \
         -I$(LKSDK)/include -I$(LKSDK)/apps/lib

LDFLAGS_ELF = -r

START_OBJ = $(LKSDK)/apps/lib/start.o
LKUI_OBJ  = $(LKSDK)/apps/lib/lkui.o

LIB_OBJS = $(START_OBJ) $(LKUI_OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

$(START_OBJ): $(LKSDK)/apps/lib/start.S
	$(CC) $(CFLAGS) -c -o $@ $<

$(LKUI_OBJ): $(LKSDK)/apps/lib/lkui.c $(LKSDK)/apps/lib/lkui.h $(LKSDK)/include/lk_app.h
	$(CC) $(CFLAGS) -c -o $@ $<
