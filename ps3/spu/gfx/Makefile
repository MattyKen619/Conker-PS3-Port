# The SPU graphics program (spu_gfx.elf -> spu_gfx.bin, embedded in the PPU app with bin2o): the
# display list interpreter (ps3/gfx/n64gfx.cpp) recording a draw stream (n64gfx_stream.cpp).
#   make -C ps3/spu/gfx
ROOT   := $(abspath $(dir $(abspath $(firstword $(MAKEFILE_LIST))))/../../..)
PS3DEV ?= /root/ps3dev
SPUCXX := $(PS3DEV)/spu/bin/spu-g++
G      := $(ROOT)/ps3/gfx
B      := build
FLAGS  := -O2 -funroll-loops --param max-unroll-times=2 -I$(PS3DEV)/spu/include -fno-strict-aliasing -std=gnu++17 -fno-exceptions -fno-rtti \
          -ffunction-sections -fdata-sections -Wall -Wno-unused-function -I$(G) -I$(ROOT)/ps3/spu -I$(ROOT)/upstream/CBFD-Recompiled/tools/N64ModernRuntime/thirdparty
DEPS   := $(wildcard $(G)/*.h) $(ROOT)/ps3/spu/spu_gfx_job.h

all: spu_gfx.bin
$(B)/%.o: $(G)/%.cpp $(DEPS) ; @mkdir -p $(B); $(SPUCXX) $(FLAGS) -c $< -o $@
$(B)/spu_gfx_main.o: spu_gfx_main.cpp $(DEPS) ; @mkdir -p $(B); $(SPUCXX) $(FLAGS) -c $< -o $@
spu_gfx.elf: $(B)/spu_gfx_main.o $(B)/n64gfx.o $(B)/n64gfx_stream.o
	$(SPUCXX) -Wl,--gc-sections -o $@ $^ -L$(PS3DEV)/spu/lib -lsputhread -lm
	@$(PS3DEV)/spu/bin/spu-size $@
spu_gfx.bin: spu_gfx.elf
	$(PS3DEV)/spu/bin/spu-strip -o $@ $<
clean: ; rm -rf $(B) spu_gfx.elf spu_gfx.bin
.PHONY: all clean
