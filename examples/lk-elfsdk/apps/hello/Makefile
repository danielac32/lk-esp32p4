APP = hello
OBJS = hello.o $(START_OBJ)
.DEFAULT_GOAL := all

include ../common.mk

all: $(APP).elf

$(APP).elf: $(OBJS)
	$(LD) $(LDFLAGS_ELF) -o $@ $(OBJS)

clean:
	rm -f hello.o $(START_OBJ) $(APP).elf

.PHONY: all clean
