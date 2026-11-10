# Amatrix build: core library, tests, tools for one target.
#
#   make                    TARGET=native (Linux/macOS host, OpenSSL)
#   make TARGET=asan        host build with AddressSanitizer/UBSan
#   make TARGET=amiga       m68k-amigaos (libnix, 68020+FPU, bsdsocket, AmiSSL)
#
# Output in build/$(TARGET)/: libolm.a, libamatrix.a, tests/<feature>, tools/*
# Tests: tests/run.py (builds what it needs).

TARGET ?= native
B      := build/$(TARGET)
OLM    := third_party/libolm
CJSON  := third_party/cjson
AMISSL := third_party/amissl-sdk

OLM_DEFS := -DOLMLIB_VERSION_MAJOR=3 -DOLMLIB_VERSION_MINOR=2 -DOLMLIB_VERSION_PATCH=16 -DOLM_STATIC_DEFINE
INC      := -Icore/include -Icore/src -I$(CJSON) -I$(OLM)/include -Itests
CORE_SRC := client.c async.c media.c recovery.c storecrypt.c keys.c crypto_recv.c crypto_send.c sync.c rooms.c roomstate.c messages.c admin.c hcache.c \
            canonjson.c http_sock.c net.c

ifeq ($(TARGET),amiga)
  export PATH := /opt/amiga/bin:$(PATH)
  CC   := m68k-amigaos-gcc
  CXX  := m68k-amigaos-g++
  AR   := m68k-amigaos-ar
  ARCH := -mcrt=nix20 -m68020 -m68881 -O2 -fomit-frame-pointer
  CFLAGS   := $(ARCH) -std=gnu99 -Wall -DAMX_AMISSL -I$(AMISSL)/include
  CXXFLAGS := $(ARCH) -std=c++11
  PLATFORM := platform_amiga.c
  LIBS     := -L$(AMISSL)/lib/AmigaOS3 -lamisslstubs -lstdc++ -lm
  STRIP    := m68k-amigaos-strip
else
  CC   ?= cc
  CXX  ?= c++
  AR   ?= ar
  ifeq ($(TARGET),asan)
    SAN := -fsanitize=address,undefined -fno-omit-frame-pointer
  endif
  CFLAGS   := -O1 -g -std=gnu99 -Wall $(SAN)
  CXXFLAGS := -O1 -g -std=c++11 $(SAN)
  PLATFORM := platform_posix.c
  LIBS     := -lssl -lcrypto -lstdc++ -lpthread $(SAN)
  STRIP    := true
endif

OLM_CXX := $(wildcard $(OLM)/src/*.cpp)
OLM_C   := $(wildcard $(OLM)/src/*.c) $(OLM)/lib/crypto-algorithms/aes.c \
           $(OLM)/lib/crypto-algorithms/sha256.c $(OLM)/lib/curve25519-donna/curve25519-donna.c
OLM_OBJ := $(patsubst $(OLM)/%,$(B)/olm/%.o,$(OLM_CXX) $(OLM_C))
CORE_OBJ := $(patsubst %,$(B)/core/%.o,$(CORE_SRC) $(PLATFORM)) $(B)/core/cJSON.c.o

FEATURES := $(notdir $(wildcard tests/features/*))
TEST_BIN := $(patsubst %,$(B)/tests/%,$(FEATURES))
TOOL_BIN := $(B)/tools/amxdiag $(B)/tools/amx-cli $(B)/examples/minimal

all: $(B)/libamatrix.a $(TEST_BIN) $(TOOL_BIN)
tests: $(TEST_BIN)

$(B)/libolm.a: $(OLM_OBJ)
	$(AR) rcs $@ $^

$(B)/libamatrix.a: $(CORE_OBJ)
	$(AR) rcs $@ $^

$(B)/olm/%.cpp.o: $(OLM)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OLM_DEFS) -I$(OLM)/include -I$(OLM)/lib -c $< -o $@

$(B)/olm/%.c.o: $(OLM)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -Wall,$(CFLAGS)) -w $(OLM_DEFS) -I$(OLM)/include -I$(OLM)/lib -c $< -o $@

$(B)/core/%.c.o: core/src/%.c core/src/amx_internal.h core/include/amatrix.h core/include/amatrix_async.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OLM_DEFS) $(INC) -c $< -o $@

$(B)/core/cJSON.c.o: $(CJSON)/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/tests/%: tests/features/%/test.c tests/mxtest.h $(B)/libamatrix.a $(B)/libolm.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< $(B)/libamatrix.a $(B)/libolm.a $(LIBS) -o $@
	$(STRIP) $@

$(B)/tools/amxdiag: tools/amxdiag.c $(B)/libamatrix.a $(B)/libolm.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OLM_DEFS) $(INC) $< $(B)/libamatrix.a $(B)/libolm.a $(LIBS) -o $@
	$(STRIP) $@

$(B)/tools/amx-cli: tools/amx-cli.c $(B)/libamatrix.a $(B)/libolm.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) $< $(B)/libamatrix.a $(B)/libolm.a $(LIBS) -o $@
	$(STRIP) $@

$(B)/examples/%: examples/%.c $(B)/libamatrix.a $(B)/libolm.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Icore/include $< $(B)/libamatrix.a $(B)/libolm.a $(LIBS) -o $@
	$(STRIP) $@

clean:
	rm -rf build

.PHONY: gui all tests clean richtext-test

# unit test of the HTML converter of the GUI, on the build machine
richtext-test:
	cc -Wall -Wextra -g -fsanitize=address,undefined -Igui gui/richtext.c tests/gui/richtext_test.c -o build/richtext-test
	build/richtext-test

# ---------------------------------------------------------------- MUI client
# (Amiga only; the MUI SDK comes from third_party/fetch-mui.sh)
MUI_INC := third_party/mui/include
GUI_SRC := gui/amatrix.c gui/mui_util.c gui/richtext.c
GUI_CFLAGS = $(CFLAGS) -Wno-attributes -Wno-pointer-sign -finput-charset=UTF-8 -fexec-charset=ISO-8859-1 \
             -I$(MUI_INC) -Icore/include -Igui

gui: $(B)/gui/Amatrix $(B)/gui/muicheck

$(MUI_INC)/libraries/mui.h:
	third_party/fetch-mui.sh

$(B)/gui/Amatrix: $(GUI_SRC) gui/mui_util.h $(MUI_INC)/libraries/mui.h $(B)/libamatrix.a $(B)/libolm.a
	@mkdir -p $(dir $@)
	$(CC) $(GUI_CFLAGS) $(GUI_SRC) $(B)/libamatrix.a $(B)/libolm.a $(LIBS) -lamiga -o $@
	$(STRIP) $@

$(B)/gui/muicheck: gui/muicheck.c gui/mui_util.c gui/mui_util.h $(MUI_INC)/libraries/mui.h
	@mkdir -p $(dir $@)
	$(CC) $(GUI_CFLAGS) gui/muicheck.c gui/mui_util.c -lamiga -o $@
	$(STRIP) $@
