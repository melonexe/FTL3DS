# Headless PC build of the test build (make TEST=1's game, with nothing drawn or played), for
# running scenario tests fast on a PC or server. Needs gcc, libpng and zlib (Linux: build-essential
# libpng-dev zlib1g-dev). Run the scenarios on it with tools/test/run_headless.py; the test server
# (tools/testserver) does both for every new commit. See BUILDING.md, "Headless tests".
#
#   make -C test/headless    ->  test/headless/build/ftl3ds_headless

CC      ?= gcc
ROOT    := $(abspath $(CURDIR)/../..)
GAME    := $(filter-out $(ROOT)/source/sound.c,$(wildcard $(ROOT)/source/*.c)) $(wildcard $(ROOT)/test/*.c)
PC      := $(CURDIR)/stubs.c $(CURDIR)/sound_stub.c $(CURDIR)/pcfs.c
OUT     := $(CURDIR)/build/ftl3ds_headless
OBJDIR  := $(CURDIR)/build/obj

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wno-unused-function -Wno-deprecated-declarations -DFTL_TEST -DFTL_HEADLESS \
           -I$(CURDIR)/include -I$(ROOT)/source -I$(ROOT)/test -include $(CURDIR)/pcfs.h
LDLIBS  := -lpng -lz -lm

OBJS    := $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(GAME) $(PC)))
vpath %.c $(ROOT)/source $(ROOT)/test $(CURDIR)

$(OUT): $(OBJS)
	$(CC) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR):
	mkdir -p $@

clean:
	rm -rf $(CURDIR)/build

-include $(OBJS:.o=.d)
.PHONY: clean
