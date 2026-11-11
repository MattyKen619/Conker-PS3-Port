# Builds the Conker PS3 app: PS3 host + CBFD-Recompiled host files + N64ModernRuntime + game code.
#   make -C ../game && make -C ../runtime && make pkg
.SUFFIXES:
# Repo root, from this Makefile's location (the build runs from build/ as well).
ROOT      := $(abspath $(dir $(abspath $(firstword $(MAKEFILE_LIST))))/..)
ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif
include $(PSL1GHT)/ppu_rules

UP        := $(ROOT)/upstream/CBFD-Recompiled
RT        := $(UP)/tools/N64ModernRuntime

# RELEASE=1: the release build ("Conker's Bad Fur Day" on the XMB, its own app): no testing or logs,
# its own data folder, the menu without the unfinished settings, the XMB icon made from the player's
# ROM (stand-in in the package: tools/release/mark_icon.py; or release/ICON0.local.png if you made one,
# not in the repository).
# PROFILE=1: the profiler build (instrumented game code, separate app on the XMB).
VERSION   := 1.0
ifeq ($(RELEASE),1)
TARGET    := conker_bfd
BUILD     := build-release
TITLE     := Conker's Bad Fur Day
APPID     := CNKR00001
GAMELIB   := $(ROOT)/game/libgame.a
PROFDEF   := -DCONKER_RELEASE=1 -DCONKER_APPID=\"$(APPID)\" -DCONKER_VERSION=\"$(VERSION)\"
LOCAL_ICON := $(wildcard $(ROOT)/release/ICON0.local.png)
LOCAL_PIC1 := $(wildcard $(ROOT)/release/PIC1.local.png)
ICON0     := $(if $(LOCAL_ICON),$(LOCAL_ICON),$(CURDIR)/$(BUILD)/icon_standin.png)
SFOXML    := $(CURDIR)/$(BUILD)/sfo.xml
else ifeq ($(PROFILE),1)
TARGET    := conkerps3prof
BUILD     := build-prof
TITLE     := Conker PS3 (profiler)
APPID     := CNKR00003
GAMELIB   := $(ROOT)/game/libgame-prof.a
PROFDEF   := -DCONKER_PROFILER_BUILD=1
else
TARGET    := conkerps3
BUILD     := build
TITLE     := Conker PS3 (graphics test)
APPID     := CNKR00005
GAMELIB   := $(ROOT)/game/libgame.a
PROFDEF   :=
endif
CONTENTID := UP0001-$(APPID)_00-0000000000000000

# Our sources, plus the portable files of CBFD-Recompiled's host.
SOURCES   := $(ROOT)/ps3/host $(ROOT)/ps3/compat $(ROOT)/ps3/gfx $(ROOT)/ps3/gfx/fpasm
# softraster.cpp (the workstation's software renderer) draws the picture checks (ps3_renderer.cpp).
EXCLUDE   :=
UPHOST    := ultra_extras overlays rom_versions null_renderer mod_api

INCLUDE   := -I$(ROOT)/ps3/host -I$(ROOT)/ps3/gfx -I$(ROOT)/ps3/include -I$(ROOT)/ps3/include/gen \
             -I$(RT)/ultramodern/include -I$(RT)/librecomp/include -I$(RT)/librecomp/include/librecomp \
             -I$(RT)/thirdparty -I$(RT)/thirdparty/concurrentqueue -I$(RT)/thirdparty/miniz \
             -I$(RT)/thirdparty/xxHash -I$(RT)/thirdparty/json/include -I$(RT)/N64Recomp/include \
             -I$(UP)/RecompiledFuncs -I$(UP)/host/src $(LIBPSL1GHT_INC)
CFLAGS    := $(PROFDEF) -DNDEBUG -O2 -fno-tree-vectorize -Wall -mcpu=cell $(MACHDEP) $(INCLUDE) -Dstatic_assert=_Static_assert
CXXFLAGS  := $(PROFDEF) -DNDEBUG -O2 -fno-tree-vectorize -mcpu=cell $(MACHDEP) $(INCLUDE) -std=gnu++20 -include ps3_std_compat.h -w
LDFLAGS   := $(MACHDEP) -Wl,-Map,$(notdir $@).map
LIBS      := $(GAMELIB) $(ROOT)/runtime/libruntime.a \
             -lrsx -lgcm_sys -lio -laudio -lsysutil -lsysmodule -llv2dbg -lnet -lrt -llv2 -lm

ifeq ($(RELEASE),1)
# The package's extra files: the stand-in icon (marked, replaced on the first start), PARAM.SFO's
# template with this version, and PIC1.PNG (the XMB background) if you made one.
PKGPREP   := release-prep
export PKGFILES := $(if $(LOCAL_PIC1),$(CURDIR)/$(BUILD)/pkgfiles,)
.PHONY: release-prep
release-prep:
	@mkdir -p $(CURDIR)/$(BUILD)/pkgfiles
	@python3 $(ROOT)/tools/release/mark_icon.py $(PS3DEV)/bin/ICON0.PNG $(CURDIR)/$(BUILD)/icon_standin.png
	@sed -e 's/01\.00/0$(VERSION)0/' $(PS3DEV)/bin/sfo.xml > $(CURDIR)/$(BUILD)/sfo.xml
	@rm -f $(CURDIR)/$(BUILD)/pkgfiles/PIC1.PNG $(CURDIR)/$(BUILD)/pkg/PIC1.PNG
	@$(if $(LOCAL_PIC1),cp $(LOCAL_PIC1) $(CURDIR)/$(BUILD)/pkgfiles/PIC1.PNG,true)
endif

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(SOURCES) $(UP)/host/src
export DEPSDIR  := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)
CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(filter-out $(EXCLUDE),$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))) $(addsuffix .cpp,$(UPHOST))
export LD       := $(CXX)
export OFILES   := spu_audio_o2.bin.o spu_gfx.bin.o $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export LIBPATHS := $(LIBPSL1GHT_LIB)
.PHONY: $(BUILD) clean pkg
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
pkg: $(BUILD) $(PKGPREP) $(OUTPUT).pkg
clean:
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).fake.self $(OUTPUT)*.pkg
else
.DEFAULT_GOAL := $(OUTPUT).self
DEPENDS := $(OFILES:.o=.d)
# The display list interpreter's vertex path uses the vector unit (see n64gfx.cpp).
n64gfx.o: CXXFLAGS += -maltivec -mabi=altivec
# The audio microcode's SPU program (ps3/spu/audio), embedded: spu_audio_o2_bin[], spu_audio_o2_bin.h.
SPUBIN := $(ROOT)/ps3/spu/audio/spu_audio_o2.bin
$(SPUBIN): FORCE
	@$(MAKE) --no-print-directory -C $(ROOT)/ps3/spu/audio
spu_audio_o2.bin: $(SPUBIN)
	cp $< $@
spu_audio_o2.bin.o: spu_audio_o2.bin
	@echo $(notdir $<)
	$(bin2o)
# The display list interpreter's SPU program (ps3/spu/gfx): spu_gfx_bin[], spu_gfx_bin.h.
SPUGFX := $(ROOT)/ps3/spu/gfx/spu_gfx.bin
$(SPUGFX): FORCE
	@$(MAKE) --no-print-directory -C $(ROOT)/ps3/spu/gfx
spu_gfx.bin: $(SPUGFX)
	cp $< $@
spu_gfx.bin.o: spu_gfx.bin
	@echo $(notdir $<)
	$(bin2o)
main.o: spu_audio_o2.bin.o spu_gfx.bin.o
main.o: CXXFLAGS += -I$(CURDIR)
.PHONY: FORCE
$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
