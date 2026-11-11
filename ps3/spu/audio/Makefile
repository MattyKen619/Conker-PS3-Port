# The SPU audio program (spu_audio.elf -> spu_audio.bin, embedded in PPU apps with bin2o).
#   make -C ps3/spu/audio
ROOT   := $(abspath $(dir $(abspath $(firstword $(MAKEFILE_LIST))))/../../..)
PS3DEV ?= /root/ps3dev
SPUCXX := $(PS3DEV)/spu/bin/spu-g++
RT     := $(ROOT)/upstream/CBFD-Recompiled/tools/N64ModernRuntime
B      := build
# -fno-strict-aliasing as every other build of the microcode (without it the results are wrong).
COMMON := -I$(PS3DEV)/spu/include -fno-strict-aliasing -std=gnu++17 -fno-exceptions -fno-rtti -w -I$(ROOT)/ps3/spu/include -I$(ROOT)/ps3/spu -I$(RT)/librecomp/include
# Two builds of the microcode: -O2 alone is ~200 KB, too big for the 256 KB local store. The -Os build
# is miscompiled (tools/spu-sim: 2 of the 6 recorded tasks differ); the game uses spu_audio_o2.
FLAGS  := -Os $(COMMON)
FLAGS2 := -O2 -finline-limit=20 $(COMMON)

all: spu_audio.bin spu_audio_o2.bin
$(B)/audio_ucode_spu.cpp: gen_ucode.py $(ROOT)/upstream/CBFD-Recompiled/RecompiledFuncs/rsp/audio_ucode.cpp
	@mkdir -p $(B); python3 $^ $@
# The SPU's copy of the vector unit code (gen_vu_impl.py), found before the runtime's.
VUIMPL := $(B)/include/librecomp/rsp_vu_impl.hpp
$(VUIMPL): gen_vu_impl.py $(RT)/librecomp/include/librecomp/rsp_vu.hpp $(RT)/librecomp/include/librecomp/rsp_vu_impl.hpp
	@mkdir -p $(dir $@); python3 $^ $(dir $@)
$(B)/audio_ucode_spu.o: $(B)/audio_ucode_spu.cpp $(ROOT)/ps3/spu/include/librecomp/rsp.hpp $(VUIMPL)
	$(SPUCXX) -I$(B)/include $(FLAGS) -c $< -o $@
$(B)/audio_ucode_spu_o2.o: $(B)/audio_ucode_spu.cpp $(ROOT)/ps3/spu/include/librecomp/rsp.hpp $(VUIMPL)
	$(SPUCXX) -I$(B)/include $(FLAGS2) -c $< -o $@
$(B)/rsp_tables.h: gen_tables.py
	@mkdir -p $(B); python3 $< $@
$(B)/spu_audio_main.o: spu_audio_main.cpp $(ROOT)/ps3/spu/include/librecomp/rsp.hpp $(ROOT)/ps3/spu/spu_audio_job.h $(B)/rsp_tables.h $(VUIMPL)
	@mkdir -p $(B); $(SPUCXX) -I$(B)/include $(FLAGS) -I$(B) -c $< -o $@
spu_audio.elf: $(B)/spu_audio_main.o $(B)/audio_ucode_spu.o
	$(SPUCXX) -Os -o $@ $^ -L$(PS3DEV)/spu/lib -lsputhread
	@$(PS3DEV)/spu/bin/spu-size $@
spu_audio.bin: spu_audio.elf
	$(PS3DEV)/spu/bin/spu-strip -o $@ $<
spu_audio_o2.elf: $(B)/spu_audio_main.o $(B)/audio_ucode_spu_o2.o
	$(SPUCXX) -Os -o $@ $^ -L$(PS3DEV)/spu/lib -lsputhread
	@$(PS3DEV)/spu/bin/spu-size $@
spu_audio_o2.bin: spu_audio_o2.elf
	$(PS3DEV)/spu/bin/spu-strip -o $@ $<
clean: ; rm -rf $(B) spu_audio*.elf spu_audio*.bin
.PHONY: all clean
