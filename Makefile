#---------------------------------------------------------------------------------
# FTL 3DS - based on the devkitPro 3DS template.
# Requires devkitPro with the 3ds-dev group plus: pacman -S 3ds-zlib 3ds-libpng 3ds-libvorbisidec
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET          := ftl3ds
BUILD           := build
SOURCES         := source
INCLUDES        := source

APP_TITLE       := FTL 3DS
APP_DESCRIPTION := FTL engine reimplementation (needs your own ftl.dat)
APP_AUTHOR      := FTL-port

#---------------------------------------------------------------------------------
ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS   := -g -Wall -O2 -mword-relocations -ffunction-sections -std=gnu11 $(ARCH)
CFLAGS   += $(INCLUDE) -D__3DS__
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# The scenario-test build: make TEST=1 builds ftl3ds_test.3dsx in build_test/, adding test/ (the
# scenario runner, see source/testhooks.h). The normal build never compiles it.
ifeq ($(TEST),1)
TARGET   := ftl3ds_test
BUILD    := build_test
SOURCES  := source test
INCLUDES := source test
APP_TITLE := FTL 3DS (test)
CFLAGS   += -DFTL_TEST
endif

LIBS     := -lcitro2d -lcitro3d -lpng16 -lz -lvorbisidec -logg -lctru -lm
LIBDIRS  := $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

export LD       := $(CC)
export OFILES   := $(CFILES:.c=.o)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifneq (,$(wildcard icon.png))
export APP_ICON := $(TOPDIR)/icon.png
else
export APP_ICON := $(CTRULIB)/default_icon.png
endif

export _3DSXDEPS  := $(OUTPUT).smdh
export _3DSXFLAGS += --smdh=$(OUTPUT).smdh

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

$(OUTPUT).3dsx : $(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf  : $(OFILES)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
