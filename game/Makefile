# Compiles the recompiled game code (upstream/CBFD-Recompiled/RecompiledFuncs) for the PS3 PPU
# into libgame.a. Run scripts/setup_upstream.sh first.
ROOT    := $(abspath ..)
UP      := $(ROOT)/upstream/CBFD-Recompiled
SRC     := $(UP)/RecompiledFuncs
PS3DEV  ?= /usr/local/ps3dev
CC      := $(PS3DEV)/ppu/bin/ppu-gcc
AR      := $(PS3DEV)/ppu/bin/ppu-ar
# setjmp.h is force-included because conker.toml injects setjmp/longjmp into two functions.
# -fno-tree-vectorize: GCC 13's automatic vectorization (on at -O2) produced wrong results in the
# RSP audio microcode on the PS3 (found with tools/ppc-harness); it is turned off for all PS3 code.
CFLAGS  := -MMD -MP -include setjmp.h -DNDEBUG -O2 -fno-tree-vectorize -mcpu=cell -std=gnu11 -fno-strict-aliasing -w \
           -I$(ROOT)/ps3/include -I$(UP)/tools/N64Recomp/include -I$(SRC)
CS      := $(wildcard $(SRC)/*.c)
# PROFILE=1: instrument every game function for the sampling profiler (ps3/host/perf.c).
ifeq ($(PROFILE),1)
CFLAGS  += -finstrument-functions
BDIR    := build-prof
LIB     := libgame-prof.a
else
BDIR    := build
LIB     := libgame.a
endif
OBJ     := $(patsubst $(SRC)/%.c,$(BDIR)/%.o,$(CS))

all: $(LIB)
$(BDIR)/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	@echo CC $*
	@$(CC) $(CFLAGS) -c $< -o $@
$(LIB): $(OBJ)
	$(AR) rcs $@ $^
clean:
	rm -rf build build-prof libgame.a libgame-prof.a
.PHONY: all clean
