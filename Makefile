#---------------------------------------------------------------------------------
.PHONY: all clean

#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output. If it is not specified, it will default
#   to the name of the current directory.
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# ROMFS is the directory containing data to be added to the RomFS
#---------------------------------------------------------------------------------
TARGET		:=	cart-ridge
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
GRAPHICS	:=	gfx
AUDIO		:=	audio
ROMFS		:=	romfs
GFXBUILD	:=	$(ROMFS)/gfx
AUDIOBUILD	:=	$(ROMFS)/audio

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-fomit-frame-pointer -ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level
# containing include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
					$(foreach dir,$(AUDIO),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))
AUDIOFILES	:=	$(foreach dir,$(AUDIO),$(notdir $(wildcard $(dir)/*.wav)))
FONTFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.ttf)))

export ROMFS_T3XFILES	:=	$(patsubst %.t3s,$(GFXBUILD)/%.t3x,$(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s,$(BUILD)/%.h,$(GFXFILES))
export ROMFS_AUDIOFILES	:=	$(patsubst %,$(AUDIOBUILD)/%,$(AUDIOFILES))
export ROMFS_FONTFILES	:=	$(patsubst %.ttf,$(GFXBUILD)/%.bcfnt,$(FONTFILES))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ROMFS)),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/romfs
else
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif
# 3dsxtool requires --smdh whenever --romfs is passed (it crashes with
# "Cannot open SMDH file!" otherwise), so build one via the %.smdh rule
# from 3ds_rules. Title/description/author feed that SMDH; the icon lookup
# below picks up icon.png (or cart-ridge.png) from the project root or gfx/
# automatically once one exists, falling back to libctru's generic icon.
export _3DSXFLAGS += --smdh=$(OUTPUT).smdh
export APP_TITLE       := Cart Ridge
export APP_DESCRIPTION := Reload by swapping game cartridges
export APP_AUTHOR      := bigtuna824

# --- optional CIA packaging (`make cia`) -----------------------------------
# Needs bannertool + makerom (devkitPro's 3dstools package) in addition to
# the base 3DS dev tools -- installable directly (no cart, no Homebrew
# Launcher needed) via FBI or similar on a console running a CFW that
# permits self-signed titles (Luma3DS, same as any 3DS already capable of
# running homebrew at all). Reuses the same .smdh built for the .3dsx below
# as the CIA's icon -- it's the same file format either way.
META         := meta
RSF_FILE     := $(CURDIR)/$(META)/cart-ridge.rsf
BANNER_IMAGE := $(CURDIR)/$(META)/banner.png
BANNER_AUDIO := $(CURDIR)/$(META)/banner.wav
APP_PRODUCT_CODE := CTR-H-CTRG
APP_UNIQUE_ID    := 0xCA271

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png) $(wildcard $(GRAPHICS)/*.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(filter %icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/$(firstword $(filter %icon.png,$(icons)))
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

.PHONY: $(BUILD) clean all cia

#---------------------------------------------------------------------------------
all: $(BUILD) $(GFXBUILD) $(ROMFS_T3XFILES) $(T3XHFILES) $(AUDIOBUILD) $(ROMFS_AUDIOFILES) $(ROMFS_FONTFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@[ -d $@ ] || mkdir -p $@

#---------------------------------------------------------------------------------
# `all` also builds the .elf/.smdh this depends on -- it's phony, so this
# always re-enters the recursive submake, same as a plain `make` does; that
# submake's own dependency tracking is what actually skips work when
# nothing changed.
#---------------------------------------------------------------------------------
cia: all $(BUILD)/banner.bnr
	@echo $(TARGET).cia
	@makerom -f cia -o $(TARGET).cia -rsf $(RSF_FILE) -target t -exefslogo \
		-elf $(OUTPUT).elf -icon $(OUTPUT).smdh -banner $(BUILD)/banner.bnr \
		-DAPP_TITLE="$(APP_TITLE)" -DAPP_PRODUCT_CODE="$(APP_PRODUCT_CODE)" \
		-DAPP_UNIQUE_ID="$(APP_UNIQUE_ID)" -DAPP_ROMFS="$(CURDIR)/$(ROMFS)"

$(BUILD)/banner.bnr: $(BANNER_IMAGE) $(BANNER_AUDIO) | $(BUILD)
	@echo banner.bnr
	@bannertool makebanner -i $(BANNER_IMAGE) -a $(BANNER_AUDIO) -o $(BUILD)/banner.bnr

$(GFXBUILD):
	@[ -d $@ ] || mkdir -p $@

$(AUDIOBUILD):
	@[ -d $@ ] || mkdir -p $@

#---------------------------------------------------------------------------------
# compile each gfx/*.t3s spritesheet spec into a romfs/gfx/*.t3x texture plus
# a build/*.h header of C2D_SpriteSheetGetImage index constants (named
# <sheetname>_<pngname>_idx) that source code includes directly.
#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x $(BUILD)/%.h : %.t3s | $(BUILD) $(GFXBUILD)
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(BUILD)/$*.d -o $(GFXBUILD)/$*.t3x

# The rule above only lists the .t3s spec itself as a prerequisite, so make
# has no way to know the atlas depends on the individual PNGs it
# references -- swapping out a sprite's art without touching the .t3s file
# would otherwise leave the stale, already-built atlas in place forever.
# tex3ds already emits a real dependency file (the -d flag above); this
# just needs to actually be read.
-include $(patsubst %.t3s,$(BUILD)/%.d,$(GFXFILES))

#---------------------------------------------------------------------------------
# audio/*.wav files are already in a usable format (16-bit PCM) so they just
# get copied into the romfs as-is, no compilation step needed.
#---------------------------------------------------------------------------------
$(AUDIOBUILD)/%.wav : %.wav | $(AUDIOBUILD)
	@echo $(notdir $<)
	@cp $< $@

#---------------------------------------------------------------------------------
# gfx/*.ttf -> romfs/gfx/*.bcfnt via mkbcfnt (ships alongside tex3ds in
# devkitPro's 3dstools package, so if tex3ds already works, this should
# too with no extra install).
#---------------------------------------------------------------------------------
$(GFXBUILD)/%.bcfnt : %.ttf | $(BUILD) $(GFXBUILD)
	@echo $(notdir $<)
	@mkbcfnt -o $(GFXBUILD)/$*.bcfnt $<

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(TARGET).cia $(ROMFS)

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
all	:	$(OUTPUT).3dsx

$(OUTPUT).3dsx	:	$(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf	:	$(OFILES)

-include $(DEPENDS)

endif
