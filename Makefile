# elks-craft - the ELKS Minecraft server
#
# Building needs an ELKS source tree, for its headers and its libc, and the
# ia16-elf cross toolchain that tree builds.  Point ELKS_TOP at the checkout:
#
#     make ELKS_TOP=/path/to/elks
#
# Nothing else has to be on PATH: the compiler lives in cross/bin and the
# linker shells out to elf2elks from elks/tools/bin, so both are added here.

ELKS_TOP ?= ../elks

CROSS_BIN ?= $(ELKS_TOP)/cross/bin
TOOLS_BIN ?= $(ELKS_TOP)/elks/tools/bin
export PATH := $(CROSS_BIN):$(TOOLS_BIN):$(PATH)

CC  = ia16-elf-gcc
LD  = ia16-elf-gcc

INCLUDES = -I$(ELKS_TOP)/elkscmd \
	   -I$(ELKS_TOP)/include \
	   -I$(ELKS_TOP)/libc/include \
	   -I$(ELKS_TOP)/elks/include

# these match what elkscmd builds its own programs with
CFLAGS = -mcmodel=small -melks-libc -mtune=i8086 -Wall -Wextra -Os \
	 -mno-segment-relocation-stuff -fno-inline \
	 -fno-builtin-printf -fno-builtin-fprintf \
	 -Wno-unused-parameter -Wno-sign-compare \
	 $(INCLUDES)

# The heap holds the generation heightmap (width*length bytes, freed once the
# block array is built) and, when there is no room for a block array, the edit
# table.  Every byte reserved here is a byte main memory cannot give to the
# world itself.  The stack only has to cover the deepest call chain, which is
# under 800 bytes - see the note on world_save_pump in src/ecworld.c for what
# happens when something forgets that.
LDFLAGS = -mcmodel=small -melks-libc -mtune=i8086 \
	  -maout-heap=12288 -maout-stack=3072

OBJS = src/elkscraft.o src/ecworld.o src/ecgzip.o

all: elkscraft

elkscraft: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(OBJS): src/elkscraft.h

clean:
	rm -f $(OBJS) elkscraft

.PHONY: all clean
