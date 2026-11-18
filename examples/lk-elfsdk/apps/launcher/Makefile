APP = launcher
OBJS = launcher.o $(LIB_OBJS)
.DEFAULT_GOAL := all

include ../common.mk

all: $(APP).elf

$(APP).elf: $(OBJS)
	$(LD) $(LDFLAGS_ELF) -o $@ $(OBJS)

clean:
	rm -f launcher.o $(LIB_OBJS) $(APP).elf

.PHONY: all clean
