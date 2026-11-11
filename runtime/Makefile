# Builds N64ModernRuntime (ultramodern + librecomp) and its dependencies for the PS3 PPU into
# libruntime.a, plus the recompiled RSP audio microcode. Run scripts/setup_upstream.sh first.
ROOT    := $(abspath ..)
UP      := $(ROOT)/upstream/CBFD-Recompiled
RT      := $(UP)/tools/N64ModernRuntime
NR      := $(RT)/N64Recomp
PS3DEV  ?= /usr/local/ps3dev
CC      := $(PS3DEV)/ppu/bin/ppu-gcc
CXX     := $(PS3DEV)/ppu/bin/ppu-g++
AR      := $(PS3DEV)/ppu/bin/ppu-ar

INC := -I$(PS3DEV)/ppu/include -I$(ROOT)/ps3/include -I$(ROOT)/ps3/include/gen \
       -I$(RT)/ultramodern/include -I$(RT)/librecomp/include -I$(RT)/librecomp/include/librecomp \
       -I$(RT)/librecomp/src -I$(RT)/thirdparty -I$(RT)/thirdparty/concurrentqueue \
       -I$(RT)/thirdparty/json/include -I$(RT)/thirdparty/o1heap -I$(RT)/thirdparty/xxHash \
       -I$(RT)/thirdparty/miniz -I$(NR)/include -I$(NR)/lib/sljit/sljit_src \
       -I$(NR)/lib/rabbitizer/tables -I$(NR)/lib/rabbitizer/include -I$(NR)/lib/rabbitizer/cplusplus/include \
       -I$(NR)/lib/fmt/include -I$(NR)/lib/tomlplusplus/include -I$(UP)/RecompiledFuncs

# SLJIT_SINGLE_THREADED: sljit (mods JIT, never run on PS3) would otherwise need pthreads.
# -fno-tree-vectorize: GCC 13's automatic vectorization (on at -O2) produced wrong results in the
# RSP audio microcode on the PS3 (found with tools/ppc-harness); it is turned off for all PS3 code.
COMMON   := -MMD -MP -DNDEBUG -O2 -fno-tree-vectorize -mcpu=cell -fno-strict-aliasing -w -DSLJIT_SINGLE_THREADED=1 $(INC)
# newlib 1.20 has no C11 static_assert macro.
CFLAGS   := $(COMMON) -std=gnu11 -Dstatic_assert=_Static_assert
CXXFLAGS := $(COMMON) -std=gnu++20 -include ps3_std_compat.h

ULTRA    := audio error_handling events extensions input mesgqueue misc_ultra renderer_context rsp \
            save_states scheduling threadqueue threads timer ultrainit
LIBRECOMP:= ai config config_option cont dp eep euc-jp extensions files flash heap math_routines \
            mods mod_events mod_hooks mod_manifest mod_config_api overlays pak patcher pi print \
            recomp save_states rsp sp ultra_stubs ultra_translation vi

SRCS := $(foreach f,$(ULTRA),$(RT)/ultramodern/src/$(f).cpp) \
        $(RT)/ultramodern/src/port_main.c \
        $(foreach f,$(LIBRECOMP),$(RT)/librecomp/src/$(f).cpp) \
        $(RT)/thirdparty/o1heap/o1heap/o1heap.c \
        $(wildcard $(RT)/thirdparty/miniz/miniz*.c) \
        $(RT)/thirdparty/xxHash/xxhash.c \
        $(NR)/src/symbol_lists.cpp $(NR)/src/analysis.cpp $(NR)/src/operations.cpp \
        $(NR)/src/cgenerator.cpp $(NR)/src/recompilation.cpp $(NR)/src/mod_symbols.cpp \
        $(NR)/LiveRecomp/live_generator.cpp $(NR)/lib/sljit/sljit_src/sljitLir.c \
        $(NR)/lib/fmt/src/format.cc \
        $(wildcard $(NR)/lib/rabbitizer/cplusplus/src/*/*.cpp) \
        $(shell find $(NR)/lib/rabbitizer/src -name '*.c' 2>/dev/null) \
        $(UP)/RecompiledFuncs/rsp/audio_ucode.cpp

OBJS := $(patsubst $(UP)/%,build/%.o,$(SRCS))

# The audio microcode is the most expensive work on the PPU: -O3 halves its time on the console
# (Audio check v2: 24.1 -> 11.9 ms per task, output identical). Same flags as that test's -O3 build.
build/RecompiledFuncs/rsp/audio_ucode.cpp.o: CXXFLAGS := $(subst -O2,-O3 -maltivec -mabi=altivec,$(CXXFLAGS))

all: libruntime.a
build/%.c.o: $(UP)/%.c
	@mkdir -p $(dir $@)
	@echo CC $(notdir $<)
	@$(CC) $(CFLAGS) -c $< -o $@
build/%.cpp.o: $(UP)/%.cpp
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@$(CXX) $(CXXFLAGS) -c $< -o $@
build/%.cc.o: $(UP)/%.cc
	@mkdir -p $(dir $@)
	@echo CXX $(notdir $<)
	@$(CXX) $(CXXFLAGS) -c $< -o $@
libruntime.a: $(OBJS)
	$(AR) rcs $@ $^
clean:
	rm -rf build libruntime.a
.PHONY: all clean

-include $(OBJS:.o=.d)
